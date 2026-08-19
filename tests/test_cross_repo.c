/*
 * test_cross_repo.c — Input, work-bound, and write-failure guards for the
 * cross-repository matching pass.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "foundation/compat.h"
#include "pipeline/pass_cross_repo.h"
#include "pipeline/pipeline_internal.h"

#include <sqlite3/sqlite3.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

typedef struct {
    char cache[256];
    char *saved_cache;
} cross_repo_fixture_t;

static bool cross_repo_fixture_begin(cross_repo_fixture_t *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    const char *saved = getenv("CBM_CACHE_DIR");
    if (saved) {
        fixture->saved_cache = strdup(saved);
        if (!fixture->saved_cache) {
            return false;
        }
    }
    snprintf(fixture->cache, sizeof(fixture->cache), "/tmp/cbm-cross-hardening-XXXXXX");
    return cbm_mkdtemp(fixture->cache) != NULL &&
           cbm_setenv("CBM_CACHE_DIR", fixture->cache, 1) == 0;
}

static void cross_repo_fixture_end(cross_repo_fixture_t *fixture) {
    if (fixture->saved_cache) {
        (void)cbm_setenv("CBM_CACHE_DIR", fixture->saved_cache, 1);
    } else {
        (void)cbm_unsetenv("CBM_CACHE_DIR");
    }
    if (fixture->cache[0]) {
        th_rmtree(fixture->cache);
    }
    free(fixture->saved_cache);
    memset(fixture, 0, sizeof(*fixture));
}

static bool cross_repo_project_path(const cross_repo_fixture_t *fixture, const char *project,
                                    char *out, size_t out_size) {
    int written = snprintf(out, out_size, "%s/%s.db", fixture->cache, project);
    return written > 0 && (size_t)written < out_size;
}

static bool cross_repo_create_project(const cross_repo_fixture_t *fixture, const char *project) {
    char path[512];
    if (!cross_repo_project_path(fixture, project, path, sizeof(path))) {
        return false;
    }
    cbm_store_t *store = cbm_store_open_path(path);
    if (!store) {
        return false;
    }
    bool ok = cbm_store_upsert_project(store, project, fixture->cache) == CBM_STORE_OK;
    cbm_store_close(store);
    return ok;
}

/* Seed one HTTP_CALLS/HANDLES pair into two exact project stores. The suffix
 * keeps node QNs unique when a source is linked to more than one target. */
static bool cross_repo_seed_http_pair(const cross_repo_fixture_t *fixture,
                                      const char *source_project, const char *target_project,
                                      const char *route_path, const char *suffix) {
    char source_path[512];
    char target_path[512];
    if (!cross_repo_project_path(fixture, source_project, source_path, sizeof(source_path)) ||
        !cross_repo_project_path(fixture, target_project, target_path, sizeof(target_path))) {
        return false;
    }
    cbm_store_t *source = cbm_store_open_path(source_path);
    cbm_store_t *target = cbm_store_open_path(target_path);
    if (!source || !target) {
        cbm_store_close(source);
        cbm_store_close(target);
        return false;
    }

    bool ok = cbm_store_upsert_project(source, source_project, fixture->cache) == CBM_STORE_OK &&
              cbm_store_upsert_project(target, target_project, fixture->cache) == CBM_STORE_OK;
    char caller_qn[256];
    char local_route_qn[256];
    char target_route_qn[256];
    char handler_qn[256];
    char route_name[128];
    char edge_props[256];
    snprintf(caller_qn, sizeof(caller_qn), "%s.call.%s", source_project, suffix);
    snprintf(local_route_qn, sizeof(local_route_qn), "%s.local-route.%s", source_project, suffix);
    snprintf(target_route_qn, sizeof(target_route_qn), "__route__GET__%s", route_path);
    snprintf(handler_qn, sizeof(handler_qn), "%s.handle.%s", target_project, suffix);
    snprintf(route_name, sizeof(route_name), "GET %s", route_path);
    snprintf(edge_props, sizeof(edge_props), "{\"url_path\":\"%s\",\"method\":\"GET\"}",
             route_path);

    cbm_node_t caller = {.project = source_project,
                         .label = "Function",
                         .name = "call_remote",
                         .qualified_name = caller_qn,
                         .file_path = "client.c"};
    cbm_node_t local_route = {.project = source_project,
                              .label = "Route",
                              .name = route_name,
                              .qualified_name = local_route_qn,
                              .file_path = "client.c"};
    int64_t caller_id = ok ? cbm_store_upsert_node(source, &caller) : 0;
    int64_t local_route_id = ok ? cbm_store_upsert_node(source, &local_route) : 0;
    cbm_edge_t http_call = {.project = source_project,
                            .source_id = caller_id,
                            .target_id = local_route_id,
                            .type = "HTTP_CALLS",
                            .properties_json = edge_props};
    ok = ok && caller_id > 0 && local_route_id > 0 && cbm_store_insert_edge(source, &http_call) > 0;

    cbm_node_t target_route = {.project = target_project,
                               .label = "Route",
                               .name = route_name,
                               .qualified_name = target_route_qn,
                               .file_path = "server.c"};
    cbm_node_t handler = {.project = target_project,
                          .label = "Function",
                          .name = "handle_remote",
                          .qualified_name = handler_qn,
                          .file_path = "server.c"};
    int64_t target_route_id = ok ? cbm_store_upsert_node(target, &target_route) : 0;
    int64_t handler_id = ok ? cbm_store_upsert_node(target, &handler) : 0;
    cbm_edge_t handles = {.project = target_project,
                          .source_id = handler_id,
                          .target_id = target_route_id,
                          .type = "HANDLES"};
    ok = ok && target_route_id > 0 && handler_id > 0 && cbm_store_insert_edge(target, &handles) > 0;

    cbm_store_close(source);
    cbm_store_close(target);
    return ok;
}

static bool cross_repo_exec(const cross_repo_fixture_t *fixture, const char *project,
                            const char *sql) {
    char path[512];
    if (!cross_repo_project_path(fixture, project, path, sizeof(path))) {
        return false;
    }
    cbm_store_t *store = cbm_store_open_path_existing(path);
    if (!store) {
        return false;
    }
    char *error = NULL;
    int rc = sqlite3_exec(cbm_store_get_db(store), sql, NULL, NULL, &error);
    sqlite3_free(error);
    cbm_store_close(store);
    return rc == SQLITE_OK;
}

static int cross_repo_count_edges(const cross_repo_fixture_t *fixture, const char *project,
                                  const char *edge_type) {
    char path[512];
    if (!cross_repo_project_path(fixture, project, path, sizeof(path))) {
        return -1;
    }
    cbm_store_t *store = cbm_store_open_path_query(path);
    if (!store) {
        return -1;
    }
    int count = cbm_store_count_edges_by_type(store, project, edge_type);
    cbm_store_close(store);
    return count;
}

TEST(cross_repo_null_target_fails_without_dereference) {
    cross_repo_fixture_t fixture;
    if (!cross_repo_fixture_begin(&fixture) ||
        !cross_repo_create_project(&fixture, "null-target-source")) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to create isolated source project");
    }

    bool rejected = false;
#if defined(_WIN32)
    const char *targets[] = {NULL};
    cbm_cross_repo_result_t result = cbm_cross_repo_match("null-target-source", targets, 1);
    rejected = result.failed;
#else
    fflush(NULL);
    pid_t child = fork();
    if (child == 0) {
        const char *targets[] = {NULL};
        cbm_cross_repo_result_t result = cbm_cross_repo_match("null-target-source", targets, 1);
        _exit(result.failed ? 0 : 2);
    }
    int status = 0;
    rejected = child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
               WEXITSTATUS(status) == 0;
#endif

    cross_repo_fixture_end(&fixture);
    ASSERT_TRUE(rejected);
    PASS();
}

TEST(cross_repo_wildcard_keeps_projects_containing_internal_tokens) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_http_pair(&fixture, "wildcard-source", "orders_config_service",
                                           "/config-orders", "a") &&
                 cross_repo_seed_http_pair(&fixture, "wildcard-source", "orders_cross_repo_service",
                                           "/cross-orders", "b") &&
                 cross_repo_seed_http_pair(&fixture, "wildcard-source", "orders-wal-service",
                                           "/wal-orders", "c") &&
                 cross_repo_seed_http_pair(&fixture, "wildcard-source", "orders-shm-service",
                                           "/shm-orders", "d");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed wildcard fixture");
    }

    const char *targets[] = {"*"};
    cbm_cross_repo_result_t result = cbm_cross_repo_match("wildcard-source", targets, 1);
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.projects_scanned, 4);
    ASSERT_EQ(result.http_edges, 4);
    PASS();
}

static bool cross_repo_seed_bounded_scan(const cross_repo_fixture_t *fixture,
                                         const char *source_project, const char *target_project) {
    enum { TEST_SCAN_ROWS = 4097 };
    char source_path[512];
    char target_path[512];
    if (!cross_repo_project_path(fixture, source_project, source_path, sizeof(source_path)) ||
        !cross_repo_project_path(fixture, target_project, target_path, sizeof(target_path))) {
        return false;
    }
    cbm_store_t *source = cbm_store_open_path(source_path);
    cbm_store_t *target = cbm_store_open_path(target_path);
    if (!source || !target) {
        cbm_store_close(source);
        cbm_store_close(target);
        return false;
    }
    bool ok = cbm_store_upsert_project(source, source_project, fixture->cache) == CBM_STORE_OK &&
              cbm_store_upsert_project(target, target_project, fixture->cache) == CBM_STORE_OK;
    cbm_node_t caller = {.project = source_project,
                         .label = "Function",
                         .name = "bounded_caller",
                         .qualified_name = "bounded.source.caller",
                         .file_path = "client.c"};
    int64_t caller_id = ok ? cbm_store_upsert_node(source, &caller) : 0;
    ok = ok && caller_id > 0 &&
         sqlite3_exec(cbm_store_get_db(source), "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
    for (int i = 0; ok && i < TEST_SCAN_ROWS; i++) {
        char name[64];
        char qn[96];
        snprintf(name, sizeof(name), "local_route_%d", i);
        snprintf(qn, sizeof(qn), "bounded.source.route.%d", i);
        cbm_node_t local_route = {.project = source_project,
                                  .label = "Route",
                                  .name = name,
                                  .qualified_name = qn,
                                  .file_path = "client.c"};
        int64_t route_id = cbm_store_upsert_node(source, &local_route);
        cbm_edge_t edge = {
            .project = source_project,
            .source_id = caller_id,
            .target_id = route_id,
            .type = "HTTP_CALLS",
            .properties_json = i == TEST_SCAN_ROWS - 1
                                   ? "{\"url_path\":\"/after-bound\",\"method\":\"GET\"}"
                                   : "{}",
        };
        ok = route_id > 0 && cbm_store_insert_edge(source, &edge) > 0;
    }
    if (ok) {
        ok = sqlite3_exec(cbm_store_get_db(source), "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    } else {
        (void)sqlite3_exec(cbm_store_get_db(source), "ROLLBACK", NULL, NULL, NULL);
    }

    cbm_node_t target_route = {.project = target_project,
                               .label = "Route",
                               .name = "GET /after-bound",
                               .qualified_name = "__route__GET__/after-bound",
                               .file_path = "server.c"};
    cbm_node_t handler = {.project = target_project,
                          .label = "Function",
                          .name = "bounded_handler",
                          .qualified_name = "bounded.target.handler",
                          .file_path = "server.c"};
    int64_t target_route_id = ok ? cbm_store_upsert_node(target, &target_route) : 0;
    int64_t handler_id = ok ? cbm_store_upsert_node(target, &handler) : 0;
    cbm_edge_t handles = {.project = target_project,
                          .source_id = handler_id,
                          .target_id = target_route_id,
                          .type = "HANDLES"};
    ok = ok && target_route_id > 0 && handler_id > 0 && cbm_store_insert_edge(target, &handles) > 0;
    cbm_store_close(source);
    cbm_store_close(target);
    return ok;
}

TEST(cross_repo_scan_bound_counts_examined_rows_not_matches) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_bounded_scan(&fixture, "bounded-source", "bounded-target");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed bounded scan fixture");
    }
    const char *target = "bounded-target";
    cbm_cross_repo_result_t result = cbm_cross_repo_match("bounded-source", &target, 1);
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.projects_scanned, 1);
    ASSERT_EQ(result.http_edges, 0);
    PASS();
}

TEST(cross_repo_propagates_delete_failure) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_http_pair(&fixture, "delete-source", "delete-target",
                                           "/delete-failure", "delete");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed delete failure fixture");
    }
    const char *target = "delete-target";
    cbm_cross_repo_result_t initial = cbm_cross_repo_match("delete-source", &target, 1);
    bool trigger_created =
        !initial.failed && initial.http_edges == 1 &&
        cross_repo_exec(&fixture, "delete-source",
                        "CREATE TRIGGER fail_cross_delete BEFORE DELETE ON edges "
                        "WHEN OLD.type = 'CROSS_HTTP_CALLS' BEGIN "
                        "SELECT RAISE(ABORT, 'forced cross delete failure'); END;");
    cbm_cross_repo_result_t failed = {0};
    if (trigger_created) {
        failed = cbm_cross_repo_match("delete-source", &target, 1);
    }
    cross_repo_fixture_end(&fixture);

    ASSERT_TRUE(trigger_created);
    ASSERT_TRUE(failed.failed);
    ASSERT_EQ(failed.http_edges, 0);
    PASS();
}

TEST(cross_repo_failed_bidirectional_insert_is_not_counted) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_http_pair(&fixture, "insert-source", "insert-target",
                                           "/insert-failure", "insert") &&
                 cross_repo_exec(&fixture, "insert-target",
                                 "CREATE TRIGGER fail_cross_insert BEFORE INSERT ON edges "
                                 "WHEN NEW.type = 'CROSS_HTTP_CALLS' BEGIN "
                                 "SELECT RAISE(ABORT, 'forced cross insert failure'); END;");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed insert failure fixture");
    }
    const char *target = "insert-target";
    cbm_cross_repo_result_t result = cbm_cross_repo_match("insert-source", &target, 1);
    cross_repo_fixture_end(&fixture);

    ASSERT_TRUE(result.failed);
    ASSERT_EQ(result.http_edges, 0);
    ASSERT_EQ(result.projects_scanned, 0);
    PASS();
}

typedef struct {
    atomic_int *cancelled;
    int fired;
} cross_repo_cancel_hook_t;

static void cross_repo_cancel_after_target_write(const char *project, const char *edge_type,
                                                 void *opaque) {
    cross_repo_cancel_hook_t *hook = opaque;
    if (strcmp(project, "cancel-target-b") == 0 && strcmp(edge_type, "CROSS_HTTP_CALLS") == 0) {
        hook->fired++;
        atomic_store_explicit(hook->cancelled, 1, memory_order_release);
    }
}

TEST(cross_repo_cancel_mid_run_keeps_completed_target_and_stops_before_later_target) {
    cross_repo_fixture_t fixture;
    bool setup =
        cross_repo_fixture_begin(&fixture) &&
        cross_repo_seed_http_pair(&fixture, "cancel-source", "cancel-target-a", "/cancel-a", "a") &&
        cross_repo_seed_http_pair(&fixture, "cancel-source", "cancel-target-b", "/cancel-b", "b") &&
        cross_repo_seed_http_pair(&fixture, "cancel-source", "cancel-target-c", "/cancel-c", "c");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed cancellation fixture");
    }

    atomic_int cancelled;
    atomic_init(&cancelled, 0);
    cross_repo_cancel_hook_t hook = {
        .cancelled = &cancelled,
    };

    const char *targets[] = {"cancel-target-c", "cancel-target-a", "cancel-target-b"};
    cbm_cross_repo_set_after_insert_hook_for_tests(cross_repo_cancel_after_target_write, &hook);
    cbm_cross_repo_result_t result =
        cbm_cross_repo_match_cancellable("cancel-source", targets, 3, &cancelled);
    cbm_cross_repo_set_after_insert_hook_for_tests(NULL, NULL);

    int completed_target_edges =
        cross_repo_count_edges(&fixture, "cancel-target-a", "CROSS_HTTP_CALLS");
    int interrupted_target_edges =
        cross_repo_count_edges(&fixture, "cancel-target-b", "CROSS_HTTP_CALLS");
    int later_target_edges =
        cross_repo_count_edges(&fixture, "cancel-target-c", "CROSS_HTTP_CALLS");
    cross_repo_fixture_end(&fixture);

    ASSERT_EQ(hook.fired, 1);
    ASSERT_TRUE(result.cancelled);
    ASSERT_TRUE(result.partial_results);
    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.projects_scanned, 1);
    ASSERT_TRUE(completed_target_edges > 0);
    ASSERT_TRUE(interrupted_target_edges > 0);
    ASSERT_EQ(later_target_edges, 0);
    PASS();
}

TEST(cross_repo_pre_cancel_preserves_existing_cross_edges) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_http_pair(&fixture, "pre-cancel-source", "pre-cancel-target",
                                           "/pre-cancel", "pre");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed pre-cancel fixture");
    }

    const char *target = "pre-cancel-target";
    cbm_cross_repo_result_t initial = cbm_cross_repo_match("pre-cancel-source", &target, 1);
    int before = cross_repo_count_edges(&fixture, "pre-cancel-source", "CROSS_HTTP_CALLS");
    atomic_int cancelled;
    atomic_init(&cancelled, 1);
    cbm_cross_repo_result_t result =
        cbm_cross_repo_match_cancellable("pre-cancel-source", &target, 1, &cancelled);
    int after = cross_repo_count_edges(&fixture, "pre-cancel-source", "CROSS_HTTP_CALLS");
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(initial.failed);
    ASSERT_TRUE(before > 0);
    ASSERT_TRUE(result.cancelled);
    ASSERT_FALSE(result.partial_results);
    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.projects_scanned, 0);
    ASSERT_EQ(after, before);
    PASS();
}

/* Add the internal "<name>::missed" miss-graph row that indexing writes into
 * the SAME db whenever a file parses partially. */
static bool cross_repo_add_missed_shadow(const cross_repo_fixture_t *fixture, const char *project) {
    char path[512];
    char shadow[256];
    if (!cross_repo_project_path(fixture, project, path, sizeof(path))) {
        return false;
    }
    snprintf(shadow, sizeof(shadow), "%s::missed", project);
    cbm_store_t *store = cbm_store_open_path(path);
    if (!store) {
        return false;
    }
    bool ok = cbm_store_upsert_project(store, shadow, fixture->cache) == CBM_STORE_OK;
    cbm_store_close(store);
    return ok;
}

/* #1609: any project that has ever recorded a parse miss carries a
 * "<name>::missed" shadow row in its own db. cr_store_has_exact_project
 * demanded count == 1 over ALL rows, so that second row made the project
 * unresolvable — as source AND as target — and the whole feature failed with
 * "not indexed" for a project that plainly was. mcp.c already solved exactly
 * this shape for list_projects in #1044; this site never learned it.
 *
 * The control is the pair without shadow rows: the tests above already prove
 * that path returns edges, so a regression here cannot hide behind a fixture
 * that never matched in the first place. */
TEST(cross_repo_accepts_project_with_missed_shadow_row_issue1609) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_http_pair(&fixture, "shadow-source", "shadow-target", "/orders",
                                           "s") &&
                 cross_repo_add_missed_shadow(&fixture, "shadow-source") &&
                 cross_repo_add_missed_shadow(&fixture, "shadow-target");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed shadow-row fixture");
    }

    const char *target = "shadow-target";
    cbm_cross_repo_result_t result = cbm_cross_repo_match("shadow-source", &target, 1);
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.projects_scanned, 1);
    ASSERT_EQ(result.http_edges, 1);
    PASS();
}

/* Seed only the client side: caller + local Route + one HTTP_CALLS edge whose
 * url_path is stored verbatim (it may be a full URL, like real extraction). */
static bool cross_repo_seed_client_call(const cross_repo_fixture_t *fixture, const char *project,
                                        const char *url_path, const char *suffix) {
    char path[512];
    if (!cross_repo_project_path(fixture, project, path, sizeof(path))) {
        return false;
    }
    cbm_store_t *store = cbm_store_open_path(path);
    if (!store) {
        return false;
    }
    bool ok = cbm_store_upsert_project(store, project, fixture->cache) == CBM_STORE_OK;
    char caller_qn[256];
    char local_route_qn[256];
    char edge_props[512];
    snprintf(caller_qn, sizeof(caller_qn), "%s.call.%s", project, suffix);
    snprintf(local_route_qn, sizeof(local_route_qn), "%s.local-route.%s", project, suffix);
    snprintf(edge_props, sizeof(edge_props), "{\"url_path\":\"%s\",\"method\":\"GET\"}", url_path);
    cbm_node_t caller = {.project = project,
                         .label = "Function",
                         .name = "call_remote",
                         .qualified_name = caller_qn,
                         .file_path = "client.c"};
    cbm_node_t local_route = {.project = project,
                              .label = "Route",
                              .name = "GET client",
                              .qualified_name = local_route_qn,
                              .file_path = "client.c"};
    int64_t caller_id = ok ? cbm_store_upsert_node(store, &caller) : 0;
    int64_t local_route_id = ok ? cbm_store_upsert_node(store, &local_route) : 0;
    cbm_edge_t http_call = {.project = project,
                            .source_id = caller_id,
                            .target_id = local_route_id,
                            .type = "HTTP_CALLS",
                            .properties_json = edge_props};
    ok = ok && caller_id > 0 && local_route_id > 0 && cbm_store_insert_edge(store, &http_call) > 0;
    cbm_store_close(store);
    return ok;
}

/* Seed a target-side Route (QN "__route__GET__<route_path>") plus one HANDLES
 * handler with caller-chosen name, file_path, and HANDLES properties. Called
 * repeatedly with the same route_path, the Route upserts to one node and the
 * handlers become competing HANDLES edges in insertion order. */
static bool cross_repo_seed_route_handler(const cross_repo_fixture_t *fixture, const char *project,
                                          const char *route_path, const char *handler_name,
                                          const char *handler_file, const char *handles_props) {
    char path[512];
    if (!cross_repo_project_path(fixture, project, path, sizeof(path))) {
        return false;
    }
    cbm_store_t *store = cbm_store_open_path(path);
    if (!store) {
        return false;
    }
    bool ok = cbm_store_upsert_project(store, project, fixture->cache) == CBM_STORE_OK;
    char route_qn[256];
    char handler_qn[256];
    char route_name[128];
    snprintf(route_qn, sizeof(route_qn), "__route__GET__%s", route_path);
    snprintf(handler_qn, sizeof(handler_qn), "%s.%s", project, handler_name);
    snprintf(route_name, sizeof(route_name), "GET %s", route_path);
    cbm_node_t route = {.project = project,
                        .label = "Route",
                        .name = route_name,
                        .qualified_name = route_qn,
                        .file_path = "server.c"};
    cbm_node_t handler = {.project = project,
                          .label = "Function",
                          .name = handler_name,
                          .qualified_name = handler_qn,
                          .file_path = handler_file};
    int64_t route_id = ok ? cbm_store_upsert_node(store, &route) : 0;
    int64_t handler_id = ok ? cbm_store_upsert_node(store, &handler) : 0;
    cbm_edge_t handles = {.project = project,
                          .source_id = handler_id,
                          .target_id = route_id,
                          .type = "HANDLES",
                          .properties_json = handles_props};
    ok = ok && route_id > 0 && handler_id > 0 && cbm_store_insert_edge(store, &handles) > 0;
    cbm_store_close(store);
    return ok;
}

/* Count edges of a type whose properties contain needle. */
static int cross_repo_count_edges_with_props(const cross_repo_fixture_t *fixture,
                                             const char *project, const char *edge_type,
                                             const char *needle) {
    char path[512];
    if (!cross_repo_project_path(fixture, project, path, sizeof(path))) {
        return -1;
    }
    cbm_store_t *store = cbm_store_open_path_query(path);
    if (!store) {
        return -1;
    }
    sqlite3_stmt *s = NULL;
    int count = -1;
    if (sqlite3_prepare_v2(cbm_store_get_db(store),
                           "SELECT count(*) FROM edges WHERE project = ?1 AND type = ?2 "
                           "AND instr(properties, ?3) > 0",
                           -1, &s, NULL) == SQLITE_OK &&
        sqlite3_bind_text(s, 1, project, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_text(s, 2, edge_type, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_text(s, 3, needle, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_step(s) == SQLITE_ROW) {
        count = sqlite3_column_int(s, 0);
    }
    sqlite3_finalize(s);
    cbm_store_close(store);
    return count;
}

/* Fleet census: 48 of 75 CROSS_HTTP_CALLS edges matched one all-placeholder
 * "/{}/{}" route (an express mount-prefix loss), 4 more matched bare "/{}".
 * Being literal-free, such templates match ANY same-arity path — regex
 * literals, filesystem paths, other services' routes. They must never
 * fuzzy-match. */
TEST(cross_repo_all_placeholder_template_never_matches) {
    cross_repo_fixture_t fixture;
    bool setup =
        cross_repo_fixture_begin(&fixture) &&
        cross_repo_seed_client_call(&fixture, "placeholder-source", "/tmp/heartbeat", "two") &&
        cross_repo_seed_client_call(&fixture, "placeholder-source", "/solo", "one") &&
        cross_repo_seed_route_handler(&fixture, "placeholder-target", "/{}/{}", "handle_pair",
                                      "server.c", "{\"handler\":\"handle_pair\"}") &&
        cross_repo_seed_route_handler(&fixture, "placeholder-target", "/{}", "handle_solo",
                                      "server.c", "{\"handler\":\"handle_solo\"}");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed all-placeholder fixture");
    }
    const char *target = "placeholder-target";
    cbm_cross_repo_result_t result = cbm_cross_repo_match("placeholder-source", &target, 1);
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.projects_scanned, 1);
    ASSERT_EQ(result.http_edges, 0);
    PASS();
}

/* Over-fix guard for the rule above: a template that keeps at least one
 * literal segment still fuzzy-matches a concrete client path. */
TEST(cross_repo_template_with_literal_segment_still_matches) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_client_call(&fixture, "literal-source", "/v2/orders/123", "lit") &&
                 cross_repo_seed_route_handler(&fixture, "literal-target", "/v2/orders/{}",
                                               "handle_order", "server.c",
                                               "{\"handler\":\"handle_order\"}");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed literal-template fixture");
    }
    const char *target = "literal-target";
    cbm_cross_repo_result_t result = cbm_cross_repo_match("literal-source", &target, 1);
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.http_edges, 1);
    PASS();
}

/* An absolute URL with no path ("https://api.ipify.org?format=json") names
 * only a host; cr_url_path turns it into "/" and it became every target's
 * root route (18/75 fleet false positives). No path, no rendezvous. */
TEST(cross_repo_host_only_absolute_url_never_matches_root) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_client_call(&fixture, "hostonly-source",
                                             "https://api.ipify.org?format=json", "ip") &&
                 cross_repo_seed_client_call(&fixture, "hostonly-source", "https://example.com/",
                                             "ex") &&
                 cross_repo_seed_route_handler(&fixture, "hostonly-target", "/", "handle_root",
                                               "server.c", "{\"handler\":\"handle_root\"}");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed host-only fixture");
    }
    const char *target = "hostonly-target";
    cbm_cross_repo_result_t result = cbm_cross_repo_match("hostonly-source", &target, 1);
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.http_edges, 0);
    PASS();
}

/* Over-fix guard for the rule above: a RELATIVE "/" client call is a
 * deliberate root-route request and must keep matching. */
TEST(cross_repo_relative_root_call_still_matches_root) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_client_call(&fixture, "relroot-source", "/", "root") &&
                 cross_repo_seed_route_handler(&fixture, "relroot-target", "/", "handle_root",
                                               "server.c", "{\"handler\":\"handle_root\"}");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed relative-root fixture");
    }
    const char *target = "relroot-target";
    cbm_cross_repo_result_t result = cbm_cross_repo_match("relroot-source", &target, 1);
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.http_edges, 1);
    PASS();
}

/* A route whose only HANDLES source is a test file has no production server:
 * LIMIT 1 used to crown files like auth_test.go as the handler. */
TEST(cross_repo_route_handled_only_by_test_file_is_unresolvable) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_client_call(&fixture, "testonly-source", "/hooks", "hk") &&
                 cross_repo_seed_route_handler(
                     &fixture, "testonly-target", "/hooks", "TestAPIKeyMiddlewareAcceptsXAPIKey",
                     "internal/transport/http/middleware/auth_test.go", "{\"handler\":\"t\"}");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed test-only handler fixture");
    }
    const char *target = "testonly-target";
    cbm_cross_repo_result_t result = cbm_cross_repo_match("testonly-source", &target, 1);
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.http_edges, 0);
    PASS();
}

/* With competing HANDLES edges the pass must prefer a non-test resolved
 * handler over both a test-file source and a non-test inline_handler
 * fallback. The test-file edge is inserted FIRST so the old LIMIT 1 pick
 * (lowest rowid) would crown it. */
TEST(cross_repo_prefers_production_handler_over_test_and_inline) {
    cross_repo_fixture_t fixture;
    bool setup =
        cross_repo_fixture_begin(&fixture) &&
        cross_repo_seed_client_call(&fixture, "prefer-source", "/pick", "pk") &&
        cross_repo_seed_route_handler(&fixture, "prefer-target", "/pick", "test",
                                      "src/tests/test_infer.py", "{\"handler\":\"test\"}") &&
        cross_repo_seed_route_handler(&fixture, "prefer-target", "/pick", "register_routes",
                                      "server.c",
                                      "{\"handler\":\"register_routes\",\"via\":\"inline_handler\"}") &&
        cross_repo_seed_route_handler(&fixture, "prefer-target", "/pick", "real_handler",
                                      "server.c", "{\"handler\":\"real_handler\"}");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed competing-handlers fixture");
    }
    const char *target = "prefer-target";
    cbm_cross_repo_result_t result = cbm_cross_repo_match("prefer-source", &target, 1);
    int named_real = cross_repo_count_edges_with_props(
        &fixture, "prefer-source", "CROSS_HTTP_CALLS", "\"target_function\":\"real_handler\"");
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.http_edges, 1);
    ASSERT_EQ(named_real, 1);
    PASS();
}

/* Template-literal client URLs ("/api/things/${id}") canonicalize their
 * interpolations to "{}" and must still rendezvous with the matching
 * templated route — but a FULLY interpolated path ("/${a}/${b}") becomes
 * "/{}/{}", which would exact-match an equally degenerate route QN and
 * bypass the fuzzy scan's literal-segment rule. Only the second may drop. */
TEST(cross_repo_interpolated_client_paths) {
    cross_repo_fixture_t fixture;
    bool setup =
        cross_repo_fixture_begin(&fixture) &&
        cross_repo_seed_client_call(&fixture, "interp-source", "/api/things/${id}", "one") &&
        cross_repo_seed_client_call(&fixture, "interp-source", "/${a}/${b}", "all") &&
        cross_repo_seed_route_handler(&fixture, "interp-target", "/api/things/{}", "handle_thing",
                                      "server.c", "{\"handler\":\"handle_thing\"}") &&
        cross_repo_seed_route_handler(&fixture, "interp-target", "/{}/{}", "handle_any", "server.c",
                                      "{\"handler\":\"handle_any\"}");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed interpolated-path fixture");
    }
    const char *target = "interp-target";
    cbm_cross_repo_result_t result = cbm_cross_repo_match("interp-source", &target, 1);
    int thing_edges = cross_repo_count_edges_with_props(
        &fixture, "interp-source", "CROSS_HTTP_CALLS", "\"target_function\":\"handle_thing\"");
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.http_edges, 1);
    ASSERT_EQ(thing_edges, 1);
    PASS();
}

/* The two rows of a bidirectional pair carry OPPOSITE meanings under the same
 * property names (forward: target_* = handler; reverse: target_* = caller).
 * Each row must say which reading applies — the unmarked reverse rows read as
 * though the caller were a handler and misled a fleet audit. */
TEST(cross_repo_rows_carry_direction_marker) {
    cross_repo_fixture_t fixture;
    bool setup = cross_repo_fixture_begin(&fixture) &&
                 cross_repo_seed_http_pair(&fixture, "direction-source", "direction-target",
                                           "/direction", "dir");
    if (!setup) {
        cross_repo_fixture_end(&fixture);
        FAIL("failed to seed direction fixture");
    }
    const char *target = "direction-target";
    cbm_cross_repo_result_t result = cbm_cross_repo_match("direction-source", &target, 1);
    int forward = cross_repo_count_edges_with_props(&fixture, "direction-source",
                                                    "CROSS_HTTP_CALLS",
                                                    "\"direction\":\"forward\"");
    int reverse = cross_repo_count_edges_with_props(&fixture, "direction-target",
                                                    "CROSS_HTTP_CALLS",
                                                    "\"direction\":\"reverse\"");
    cross_repo_fixture_end(&fixture);

    ASSERT_FALSE(result.failed);
    ASSERT_EQ(result.http_edges, 1);
    ASSERT_EQ(forward, 1);
    ASSERT_EQ(reverse, 1);
    PASS();
}

SUITE(cross_repo) {
    RUN_TEST(cross_repo_accepts_project_with_missed_shadow_row_issue1609);
    RUN_TEST(cross_repo_null_target_fails_without_dereference);
    RUN_TEST(cross_repo_wildcard_keeps_projects_containing_internal_tokens);
    RUN_TEST(cross_repo_scan_bound_counts_examined_rows_not_matches);
    RUN_TEST(cross_repo_propagates_delete_failure);
    RUN_TEST(cross_repo_failed_bidirectional_insert_is_not_counted);
    RUN_TEST(cross_repo_cancel_mid_run_keeps_completed_target_and_stops_before_later_target);
    RUN_TEST(cross_repo_pre_cancel_preserves_existing_cross_edges);
    RUN_TEST(cross_repo_all_placeholder_template_never_matches);
    RUN_TEST(cross_repo_template_with_literal_segment_still_matches);
    RUN_TEST(cross_repo_host_only_absolute_url_never_matches_root);
    RUN_TEST(cross_repo_relative_root_call_still_matches_root);
    RUN_TEST(cross_repo_route_handled_only_by_test_file_is_unresolvable);
    RUN_TEST(cross_repo_prefers_production_handler_over_test_and_inline);
    RUN_TEST(cross_repo_interpolated_client_paths);
    RUN_TEST(cross_repo_rows_carry_direction_marker);
}
