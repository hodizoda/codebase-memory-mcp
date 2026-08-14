/*
 * test_cross_project_links.c — Contract tests for the cross_project_links MCP
 * tool: CROSS_* edges are listed correctly, summary_only returns aggregates
 * without rows, offset pages return every link exactly once, and the empty
 * case says HOW to produce links instead of failing silently.
 */
#include "test_framework.h"
#include "test_helpers.h"

#include "foundation/compat.h"
#include "mcp/mcp.h"
#include "store/store.h"
#include <yyjson/yyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char cache[256];
    char *saved_cache;
} xlink_fixture_t;

static bool xlink_fixture_begin(xlink_fixture_t *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    const char *saved = getenv("CBM_CACHE_DIR");
    if (saved) {
        fixture->saved_cache = strdup(saved);
        if (!fixture->saved_cache) {
            return false;
        }
    }
    snprintf(fixture->cache, sizeof(fixture->cache), "/tmp/cbm-xlink-tool-XXXXXX");
    return cbm_mkdtemp(fixture->cache) != NULL &&
           cbm_setenv("CBM_CACHE_DIR", fixture->cache, 1) == 0;
}

static void xlink_fixture_end(xlink_fixture_t *fixture) {
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

static cbm_store_t *xlink_open_project(const xlink_fixture_t *fixture, const char *project) {
    char path[512];
    int written = snprintf(path, sizeof(path), "%s/%s.db", fixture->cache, project);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return NULL;
    }
    cbm_store_t *store = cbm_store_open_path(path);
    if (!store) {
        return NULL;
    }
    if (cbm_store_upsert_project(store, project, fixture->cache) != CBM_STORE_OK) {
        cbm_store_close(store);
        return NULL;
    }
    return store;
}

/* Seed one CROSS_* link the way pass_cross_repo's insert_cross_edge stores it:
 * a local source Function, a local Route/Channel target node, and an edge whose
 * properties carry the remote side (target_project/target_function/target_file)
 * plus the protocol identifier under via_key. */
static bool xlink_seed_link(cbm_store_t *store, const char *project, const char *src_qn,
                            const char *edge_type, const char *target_project,
                            const char *target_function, const char *via_key,
                            const char *via_val) {
    char route_qn[256];
    snprintf(route_qn, sizeof(route_qn), "%s.route", src_qn);

    cbm_node_t source = {.project = project,
                         .label = "Function",
                         .name = "caller",
                         .qualified_name = src_qn,
                         .file_path = "src/client.ts"};
    cbm_node_t route = {.project = project,
                        .label = "Route",
                        .name = "route",
                        .qualified_name = route_qn,
                        .file_path = "src/client.ts"};
    int64_t source_id = cbm_store_upsert_node(store, &source);
    int64_t route_id = cbm_store_upsert_node(store, &route);
    if (source_id <= 0 || route_id <= 0) {
        return false;
    }

    char props[512];
    snprintf(props, sizeof(props),
             "{\"target_project\":\"%s\",\"target_function\":\"%s\","
             "\"target_file\":\"api/handler.ts\",\"%s\":\"%s\"}",
             target_project, target_function, via_key, via_val);
    cbm_edge_t edge = {.project = project,
                       .source_id = source_id,
                       .target_id = route_id,
                       .type = edge_type,
                       .properties_json = props};
    return cbm_store_insert_edge(store, &edge) > 0;
}

/* Seed a plain intra-project edge that MUST NOT count as a cross-project link. */
static bool xlink_seed_local_call(cbm_store_t *store, const char *project) {
    cbm_node_t a = {.project = project,
                    .label = "Function",
                    .name = "local_a",
                    .qualified_name = "local.a",
                    .file_path = "src/local.ts"};
    cbm_node_t b = {.project = project,
                    .label = "Function",
                    .name = "local_b",
                    .qualified_name = "local.b",
                    .file_path = "src/local.ts"};
    int64_t a_id = cbm_store_upsert_node(store, &a);
    int64_t b_id = cbm_store_upsert_node(store, &b);
    if (a_id <= 0 || b_id <= 0) {
        return false;
    }
    cbm_edge_t edge = {
        .project = project, .source_id = a_id, .target_id = b_id, .type = "CALLS"};
    return cbm_store_insert_edge(store, &edge) > 0;
}

/* Pull content[0].text out of a tool-result envelope (mirrors test_mcp.c's
 * extract_text_content). The envelope repeats the tree text inside
 * structuredContent, so occurrence COUNTS must run on the extracted text. */
static char *xlink_text_content(const char *mcp_result) {
    if (!mcp_result) {
        return NULL;
    }
    yyjson_doc *doc = yyjson_read(mcp_result, strlen(mcp_result), 0);
    if (!doc) {
        return strdup(mcp_result);
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *content = yyjson_obj_get(root, "content");
    yyjson_val *item = content && yyjson_is_arr(content) ? yyjson_arr_get(content, 0) : NULL;
    yyjson_val *text = item ? yyjson_obj_get(item, "text") : NULL;
    const char *str = text ? yyjson_get_str(text) : NULL;
    char *result = str ? strdup(str) : strdup(mcp_result);
    yyjson_doc_free(doc);
    return result;
}

/* Count non-overlapping occurrences of needle in haystack. */
static int xlink_count_occurrences(const char *haystack, const char *needle) {
    int count = 0;
    size_t step = strlen(needle);
    const char *p = haystack;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += step;
    }
    return count;
}

TEST(cross_links_lists_seeded_edges) {
    xlink_fixture_t fixture;
    if (!xlink_fixture_begin(&fixture)) {
        FAIL("fixture setup failed");
    }
    const char *project = "xlink-list-src";
    cbm_store_t *store = xlink_open_project(&fixture, project);
    if (!store) {
        xlink_fixture_end(&fixture);
        FAIL("store setup failed");
    }
    bool seeded =
        xlink_seed_link(store, project, "xla.charge", "CROSS_HTTP_CALLS", "payments",
                        "handleCharge", "url_path", "/v1/charge") &&
        xlink_seed_link(store, project, "xla.refund", "CROSS_HTTP_CALLS", "payments",
                        "handleRefund", "url_path", "/v1/refund") &&
        xlink_seed_link(store, project, "xla.notify", "CROSS_CHANNEL", "notifier",
                        "onOrderPlaced", "channel_name", "order.placed") &&
        xlink_seed_local_call(store, project);
    cbm_store_close(store);
    if (!seeded) {
        xlink_fixture_end(&fixture);
        FAIL("seeding failed");
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char args[256];
    snprintf(args, sizeof(args), "{\"project\":\"%s\"}", project);
    char *r = cbm_mcp_handle_tool(srv, "cross_project_links", args);
    ASSERT_NOT_NULL(r);

    /* The local CALLS edge must not count: exactly the 3 CROSS_* links. */
    ASSERT_NOT_NULL(strstr(r, "total: 3"));
    ASSERT_NOT_NULL(strstr(r, "CROSS_HTTP_CALLS -> payments"));
    ASSERT_NOT_NULL(strstr(r, "CROSS_CHANNEL -> notifier"));
    ASSERT_NOT_NULL(strstr(r, "xla.charge"));
    ASSERT_NOT_NULL(strstr(r, "xla.refund"));
    ASSERT_NOT_NULL(strstr(r, "xla.notify"));
    ASSERT_NOT_NULL(strstr(r, "handleCharge"));
    ASSERT_NOT_NULL(strstr(r, "/v1/charge"));
    ASSERT_NOT_NULL(strstr(r, "order.placed"));
    ASSERT_NULL(strstr(r, "local.a"));
    ASSERT_NOT_NULL(strstr(r, "has_more: false"));
    ASSERT_NOT_NULL(strstr(r, "\"isError\":false"));
    free(r);

    /* Protocol filter narrows to the channel link only. */
    snprintf(args, sizeof(args), "{\"project\":\"%s\",\"protocol\":\"channel\"}", project);
    r = cbm_mcp_handle_tool(srv, "cross_project_links", args);
    ASSERT_NOT_NULL(r);
    ASSERT_NOT_NULL(strstr(r, "total: 1"));
    ASSERT_NOT_NULL(strstr(r, "xla.notify"));
    ASSERT_NULL(strstr(r, "xla.charge"));
    free(r);

    /* Unknown protocol fails closed with the accepted values named. */
    snprintf(args, sizeof(args), "{\"project\":\"%s\",\"protocol\":\"carrier-pigeon\"}", project);
    r = cbm_mcp_handle_tool(srv, "cross_project_links", args);
    ASSERT_NOT_NULL(r);
    ASSERT_NOT_NULL(strstr(r, "protocol must be one of"));
    ASSERT_NOT_NULL(strstr(r, "\"isError\":true"));
    free(r);

    cbm_mcp_server_free(srv);
    xlink_fixture_end(&fixture);
    PASS();
}

TEST(cross_links_summary_only_returns_counts_not_rows) {
    xlink_fixture_t fixture;
    if (!xlink_fixture_begin(&fixture)) {
        FAIL("fixture setup failed");
    }
    const char *project = "xlink-summary-src";
    cbm_store_t *store = xlink_open_project(&fixture, project);
    if (!store) {
        xlink_fixture_end(&fixture);
        FAIL("store setup failed");
    }
    bool seeded =
        xlink_seed_link(store, project, "xls.charge", "CROSS_HTTP_CALLS", "payments",
                        "handleCharge", "url_path", "/v1/charge") &&
        xlink_seed_link(store, project, "xls.refund", "CROSS_HTTP_CALLS", "payments",
                        "handleRefund", "url_path", "/v1/refund") &&
        xlink_seed_link(store, project, "xls.notify", "CROSS_CHANNEL", "notifier",
                        "onOrderPlaced", "channel_name", "order.placed");
    cbm_store_close(store);
    if (!seeded) {
        xlink_fixture_end(&fixture);
        FAIL("seeding failed");
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char args[256];
    snprintf(args, sizeof(args), "{\"project\":\"%s\",\"summary_only\":true}", project);
    char *r = cbm_mcp_handle_tool(srv, "cross_project_links", args);
    ASSERT_NOT_NULL(r);

    ASSERT_NOT_NULL(strstr(r, "total: 3"));
    ASSERT_NOT_NULL(strstr(r, "by_protocol: 2"));
    ASSERT_NOT_NULL(strstr(r, "project_pairs: 2"));
    ASSERT_NOT_NULL(strstr(r, "CROSS_HTTP_CALLS 2"));
    ASSERT_NOT_NULL(strstr(r, "CROSS_CHANNEL 1"));
    ASSERT_NOT_NULL(strstr(r, "payments"));
    ASSERT_NOT_NULL(strstr(r, "notifier"));
    /* Aggregates only — no link rows, no row identifiers. */
    ASSERT_NULL(strstr(r, "xls.charge"));
    ASSERT_NULL(strstr(r, "/v1/charge"));
    ASSERT_NULL(strstr(r, "has_more"));
    ASSERT_NOT_NULL(strstr(r, "\"isError\":false"));
    free(r);

    cbm_mcp_server_free(srv);
    xlink_fixture_end(&fixture);
    PASS();
}

TEST(cross_links_pagination_returns_each_row_exactly_once) {
    xlink_fixture_t fixture;
    if (!xlink_fixture_begin(&fixture)) {
        FAIL("fixture setup failed");
    }
    const char *project = "xlink-page-src";
    enum { XLINK_TEST_LINKS = 7, XLINK_TEST_PAGE = 3 };
    cbm_store_t *store = xlink_open_project(&fixture, project);
    if (!store) {
        xlink_fixture_end(&fixture);
        FAIL("store setup failed");
    }
    bool seeded = true;
    for (int i = 0; i < XLINK_TEST_LINKS && seeded; i++) {
        char src_qn[64];
        char via[64];
        snprintf(src_qn, sizeof(src_qn), "xlp.caller_q%d", i);
        snprintf(via, sizeof(via), "/v1/endpoint%d", i);
        seeded = xlink_seed_link(store, project, src_qn, "CROSS_HTTP_CALLS", "payments",
                                 "handleIt", "url_path", via);
    }
    cbm_store_close(store);
    if (!seeded) {
        xlink_fixture_end(&fixture);
        FAIL("seeding failed");
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    /* Collect all pages of 3 into one buffer. */
    char pages[16384];
    pages[0] = '\0';
    size_t pages_len = 0;
    int page_count = 0;
    for (int offset = 0; offset < XLINK_TEST_LINKS; offset += XLINK_TEST_PAGE) {
        char args[256];
        snprintf(args, sizeof(args),
                 "{\"project\":\"%s\",\"limit\":%d,\"offset\":%d}", project, XLINK_TEST_PAGE,
                 offset);
        char *r = cbm_mcp_handle_tool(srv, "cross_project_links", args);
        ASSERT_NOT_NULL(r);
        ASSERT_NOT_NULL(strstr(r, "total: 7"));
        bool last_page = offset + XLINK_TEST_PAGE >= XLINK_TEST_LINKS;
        ASSERT_NOT_NULL(strstr(r, last_page ? "has_more: false" : "has_more: true"));
        char *page_text = xlink_text_content(r);
        free(r);
        ASSERT_NOT_NULL(page_text);
        size_t rlen = strlen(page_text);
        if (pages_len + rlen + 1 >= sizeof(pages)) {
            free(page_text);
            cbm_mcp_server_free(srv);
            xlink_fixture_end(&fixture);
            FAIL("page buffer too small");
        }
        memcpy(pages + pages_len, page_text, rlen + 1);
        pages_len += rlen;
        page_count++;
        free(page_text);
    }
    ASSERT_EQ(page_count, 3);

    /* Exactly-once contract: every seeded link appears on exactly one page —
     * no duplicates, no gaps. */
    for (int i = 0; i < XLINK_TEST_LINKS; i++) {
        char src_qn[64];
        snprintf(src_qn, sizeof(src_qn), "xlp.caller_q%d", i);
        ASSERT_EQ(xlink_count_occurrences(pages, src_qn), 1);
    }

    cbm_mcp_server_free(srv);
    xlink_fixture_end(&fixture);
    PASS();
}

TEST(cross_links_empty_result_explains_cross_repo_indexing) {
    xlink_fixture_t fixture;
    if (!xlink_fixture_begin(&fixture)) {
        FAIL("fixture setup failed");
    }
    const char *project = "xlink-empty-src";
    cbm_store_t *store = xlink_open_project(&fixture, project);
    if (!store) {
        xlink_fixture_end(&fixture);
        FAIL("store setup failed");
    }
    bool seeded = xlink_seed_local_call(store, project);
    cbm_store_close(store);
    if (!seeded) {
        xlink_fixture_end(&fixture);
        FAIL("seeding failed");
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char args[256];
    snprintf(args, sizeof(args), "{\"project\":\"%s\"}", project);
    char *r = cbm_mcp_handle_tool(srv, "cross_project_links", args);
    ASSERT_NOT_NULL(r);

    /* A graph without CROSS_* edges is a SUCCESS with actionable guidance —
     * the old tool's silent empty response hid two months of breakage. */
    ASSERT_NOT_NULL(strstr(r, "total: 0"));
    ASSERT_NOT_NULL(strstr(r, "cross-repo-intelligence"));
    ASSERT_NOT_NULL(strstr(r, "index_repository"));
    ASSERT_NOT_NULL(strstr(r, "target_projects"));
    ASSERT_NOT_NULL(strstr(r, "\"isError\":false"));
    free(r);

    /* Filters that exclude everything name the filters, not the indexing. */
    snprintf(args, sizeof(args), "{\"project\":\"%s\",\"protocol\":\"grpc\"}", project);
    r = cbm_mcp_handle_tool(srv, "cross_project_links", args);
    ASSERT_NOT_NULL(r);
    ASSERT_NOT_NULL(strstr(r, "total: 0"));
    ASSERT_NOT_NULL(strstr(r, "\"isError\":false"));
    free(r);

    cbm_mcp_server_free(srv);
    xlink_fixture_end(&fixture);
    PASS();
}

SUITE(cross_project_links) {
    RUN_TEST(cross_links_lists_seeded_edges);
    RUN_TEST(cross_links_summary_only_returns_counts_not_rows);
    RUN_TEST(cross_links_pagination_returns_each_row_exactly_once);
    RUN_TEST(cross_links_empty_result_explains_cross_repo_indexing);
}
