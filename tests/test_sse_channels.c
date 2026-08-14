/*
 * test_sse_channels.c — SSE (Server-Sent Events) channel extraction tests.
 *
 * Exercises cbm_extract_channels() (via cbm_extract_file) on JS/TS, Python,
 * and Go snippets, verifying "sse"-transport channel records keyed by the
 * event-stream URL path: EventSource / fetch-event-source clients, express
 * inline handlers, sse-starlette / StreamingResponse emitters, sseclient,
 * net/http handlers, and r3labs/sse clients.  Negative cases pin down that
 * comments and unrelated "text/event-stream" strings emit nothing.
 */
#include "test_framework.h"
#include "cbm.h"

/* ── Helpers ───────────────────────────────────────────────────── */

/* Check if an "sse"-transport channel with the given name and direction exists. */
static int has_sse_channel(CBMFileResult *r, const char *name, CBMChannelDirection dir) {
    for (int i = 0; i < r->channels.count; i++) {
        CBMChannel *ch = &r->channels.items[i];
        if (ch->transport && strcmp(ch->transport, "sse") == 0 && ch->channel_name &&
            strcmp(ch->channel_name, name) == 0 && ch->direction == dir)
            return 1;
    }
    return 0;
}

/* Count channels with the "sse" transport. */
static int count_sse_channels(CBMFileResult *r) {
    int count = 0;
    for (int i = 0; i < r->channels.count; i++) {
        if (r->channels.items[i].transport && strcmp(r->channels.items[i].transport, "sse") == 0)
            count++;
    }
    return count;
}

/* Convenience: extract, return result. Caller frees. */
static CBMFileResult *extract(const char *src, CBMLanguage lang, const char *proj,
                              const char *path) {
    CBMFileResult *r = cbm_extract_file(src, (int)strlen(src), lang, proj, path, 0, NULL, NULL);
    return r;
}

/* ═══════════════════════════════════════════════════════════════════
 * Group A: JS/TS listeners
 * ═══════════════════════════════════════════════════════════════════ */

TEST(sse_js_eventsource_listener) {
    CBMFileResult *r = extract(
        "const es = new EventSource(\"/events\");\n"
        "es.onmessage = (e) => console.log(e.data);\n",
        CBM_LANG_JAVASCRIPT, "t", "client.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_sse_channel(r, "/events", CBM_CHANNEL_LISTEN));
    cbm_free_result(r);
    PASS();
}

/* Full URL: channel key must be the path only, with query stripped. */
TEST(sse_js_eventsource_full_url) {
    CBMFileResult *r = extract(
        "const es = new EventSource(\"http://api.local:8080/stream?token=abc\");\n",
        CBM_LANG_JAVASCRIPT, "t", "client.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_sse_channel(r, "/stream", CBM_CHANNEL_LISTEN));
    cbm_free_result(r);
    PASS();
}

/* URL supplied through a module-level const (constant-table resolution). */
TEST(sse_js_eventsource_const_url) {
    CBMFileResult *r = extract(
        "const FEED_URL = \"/feed\";\n"
        "const es = new EventSource(FEED_URL);\n",
        CBM_LANG_JAVASCRIPT, "t", "client.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_sse_channel(r, "/feed", CBM_CHANNEL_LISTEN));
    cbm_free_result(r);
    PASS();
}

/* @microsoft/fetch-event-source client. */
TEST(sse_js_fetch_event_source_listener) {
    CBMFileResult *r = extract(
        "import { fetchEventSource } from \"@microsoft/fetch-event-source\";\n"
        "fetchEventSource(\"/notifications\", { onmessage(ev) { render(ev); } });\n",
        CBM_LANG_TYPESCRIPT, "t", "client.ts");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_sse_channel(r, "/notifications", CBM_CHANNEL_LISTEN));
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Group B: JS emitters (inline express handler)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(sse_js_express_emitter) {
    CBMFileResult *r = extract(
        "const app = require(\"express\")();\n"
        "app.get(\"/events\", (req, res) => {\n"
        "    res.writeHead(200, { \"Content-Type\": \"text/event-stream\" });\n"
        "    res.write(\"data: hello\\n\\n\");\n"
        "});\n",
        CBM_LANG_JAVASCRIPT, "t", "server.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_sse_channel(r, "/events", CBM_CHANNEL_EMIT));
    cbm_free_result(r);
    PASS();
}

/* setHeader variant. */
TEST(sse_js_express_set_header_emitter) {
    CBMFileResult *r = extract(
        "router.get(\"/live\", (req, res) => {\n"
        "    res.setHeader(\"Content-Type\", \"text/event-stream\");\n"
        "    res.flushHeaders();\n"
        "});\n",
        CBM_LANG_JAVASCRIPT, "t", "server.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_sse_channel(r, "/live", CBM_CHANNEL_EMIT));
    cbm_free_result(r);
    PASS();
}

/* Negative: the media type only appears in a comment — no channel. */
TEST(sse_js_no_channel_from_comment) {
    CBMFileResult *r = extract(
        "app.get(\"/data\", (req, res) => {\n"
        "    // not an SSE endpoint, text/event-stream only in this comment\n"
        "    res.json({ ok: true });\n"
        "});\n",
        CBM_LANG_JAVASCRIPT, "t", "server.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_EQ(count_sse_channels(r), 0);
    cbm_free_result(r);
    PASS();
}

/* Negative: HTTP *client* .get() carrying the media type in request headers
 * (axios/got/ky) must not be recorded as an emitter — that would invert the
 * edge direction. */
TEST(sse_js_no_channel_from_axios_get) {
    CBMFileResult *r = extract(
        "const res = axios.get(\"/stream\", {\n"
        "    headers: { Accept: \"text/event-stream\" },\n"
        "    responseType: \"stream\",\n"
        "});\n",
        CBM_LANG_JAVASCRIPT, "t", "client.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_EQ(count_sse_channels(r), 0);
    cbm_free_result(r);
    PASS();
}

/* Negative: unrelated module-level string constant — no channel. */
TEST(sse_js_no_channel_from_unrelated_const) {
    CBMFileResult *r = extract(
        "const MIME = \"text/event-stream\";\n"
        "console.log(MIME);\n",
        CBM_LANG_JAVASCRIPT, "t", "misc.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_EQ(count_sse_channels(r), 0);
    cbm_free_result(r);
    PASS();
}

/* Negative: header set inside a plain function (handler by reference is not
 * pairable with a route in JS) — no channel rather than a wrong-keyed one. */
TEST(sse_js_no_channel_without_route) {
    CBMFileResult *r = extract(
        "function streamHandler(req, res) {\n"
        "    res.setHeader(\"Content-Type\", \"text/event-stream\");\n"
        "}\n",
        CBM_LANG_JAVASCRIPT, "t", "server.js");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_EQ(count_sse_channels(r), 0);
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Group C: Python
 * ═══════════════════════════════════════════════════════════════════ */

/* sse-starlette: EventSourceResponse inside a routed FastAPI handler. */
TEST(sse_py_event_source_response_emitter) {
    CBMFileResult *r = extract(
        "from sse_starlette.sse import EventSourceResponse\n"
        "\n"
        "@router.get(\"/stream\")\n"
        "async def stream_events():\n"
        "    return EventSourceResponse(event_generator())\n",
        CBM_LANG_PYTHON, "t", "api.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_sse_channel(r, "/stream", CBM_CHANNEL_EMIT));
    cbm_free_result(r);
    PASS();
}

/* StreamingResponse with media_type="text/event-stream". */
TEST(sse_py_streaming_response_emitter) {
    CBMFileResult *r = extract(
        "@app.get(\"/updates\")\n"
        "def updates():\n"
        "    return StreamingResponse(gen(), media_type=\"text/event-stream\")\n",
        CBM_LANG_PYTHON, "t", "api.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_sse_channel(r, "/updates", CBM_CHANNEL_EMIT));
    cbm_free_result(r);
    PASS();
}

/* Flask: Response(..., mimetype=...) under @app.route. */
TEST(sse_py_flask_route_emitter) {
    CBMFileResult *r = extract(
        "@app.route(\"/ticker\")\n"
        "def ticker():\n"
        "    return Response(generate(), mimetype=\"text/event-stream\")\n",
        CBM_LANG_PYTHON, "t", "api.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_sse_channel(r, "/ticker", CBM_CHANNEL_EMIT));
    cbm_free_result(r);
    PASS();
}

/* sseclient listener keyed by URL path. */
TEST(sse_py_sseclient_listener) {
    CBMFileResult *r = extract(
        "import sseclient\n"
        "\n"
        "def follow():\n"
        "    client = sseclient.SSEClient(\"http://api.svc:8000/events\")\n"
        "    for event in client:\n"
        "        handle(event)\n",
        CBM_LANG_PYTHON, "t", "consumer.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_sse_channel(r, "/events", CBM_CHANNEL_LISTEN));
    cbm_free_result(r);
    PASS();
}

/* Negative: EventSourceResponse in an unrouted helper — no path, no channel. */
TEST(sse_py_no_channel_without_route) {
    CBMFileResult *r = extract(
        "def make_response():\n"
        "    return EventSourceResponse(gen())\n",
        CBM_LANG_PYTHON, "t", "helpers.py");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_EQ(count_sse_channels(r), 0);
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Group D: Go
 * ═══════════════════════════════════════════════════════════════════ */

/* Handler registered by reference; keyed via the same-file HandleFunc table. */
TEST(sse_go_handler_emitter) {
    CBMFileResult *r = extract(
        "package main\n"
        "\n"
        "import \"net/http\"\n"
        "\n"
        "func sseHandler(w http.ResponseWriter, r *http.Request) {\n"
        "    w.Header().Set(\"Content-Type\", \"text/event-stream\")\n"
        "    w.Write([]byte(\"data: hi\\n\\n\"))\n"
        "}\n"
        "\n"
        "func main() {\n"
        "    http.HandleFunc(\"/events\", sseHandler)\n"
        "}\n",
        CBM_LANG_GO, "t", "server.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_sse_channel(r, "/events", CBM_CHANNEL_EMIT));
    cbm_free_result(r);
    PASS();
}

/* Inline closure handler; keyed via the enclosing registration call. */
TEST(sse_go_inline_emitter) {
    CBMFileResult *r = extract(
        "package main\n"
        "\n"
        "import \"net/http\"\n"
        "\n"
        "func main() {\n"
        "    http.HandleFunc(\"/ticker\", func(w http.ResponseWriter, r *http.Request) {\n"
        "        w.Header().Set(\"Content-Type\", \"text/event-stream\")\n"
        "    })\n"
        "}\n",
        CBM_LANG_GO, "t", "server.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_sse_channel(r, "/ticker", CBM_CHANNEL_EMIT));
    cbm_free_result(r);
    PASS();
}

/* r3labs/sse client. */
TEST(sse_go_newclient_listener) {
    CBMFileResult *r = extract(
        "package main\n"
        "\n"
        "import sse \"github.com/r3labs/sse/v2\"\n"
        "\n"
        "func subscribe() {\n"
        "    client := sse.NewClient(\"http://feeds.svc/live\")\n"
        "    client.Subscribe(\"messages\", handler)\n"
        "}\n",
        CBM_LANG_GO, "t", "consumer.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT(has_sse_channel(r, "/live", CBM_CHANNEL_LISTEN));
    cbm_free_result(r);
    PASS();
}

/* Negative: header set in a function with no route registration anywhere. */
TEST(sse_go_no_channel_without_route) {
    CBMFileResult *r = extract(
        "package main\n"
        "\n"
        "import \"net/http\"\n"
        "\n"
        "func helper(w http.ResponseWriter) {\n"
        "    w.Header().Set(\"Content-Type\", \"text/event-stream\")\n"
        "}\n",
        CBM_LANG_GO, "t", "helper.go");
    ASSERT_NOT_NULL(r);
    ASSERT_FALSE(r->has_error);
    ASSERT_EQ(count_sse_channels(r), 0);
    cbm_free_result(r);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════
 * Suite
 * ═══════════════════════════════════════════════════════════════════ */

SUITE(sse_channels) {
    /* Initialize extraction library */
    cbm_init();

    /* JS/TS listeners */
    RUN_TEST(sse_js_eventsource_listener);
    RUN_TEST(sse_js_eventsource_full_url);
    RUN_TEST(sse_js_eventsource_const_url);
    RUN_TEST(sse_js_fetch_event_source_listener);

    /* JS emitters + negatives */
    RUN_TEST(sse_js_express_emitter);
    RUN_TEST(sse_js_express_set_header_emitter);
    RUN_TEST(sse_js_no_channel_from_comment);
    RUN_TEST(sse_js_no_channel_from_axios_get);
    RUN_TEST(sse_js_no_channel_from_unrelated_const);
    RUN_TEST(sse_js_no_channel_without_route);

    /* Python */
    RUN_TEST(sse_py_event_source_response_emitter);
    RUN_TEST(sse_py_streaming_response_emitter);
    RUN_TEST(sse_py_flask_route_emitter);
    RUN_TEST(sse_py_sseclient_listener);
    RUN_TEST(sse_py_no_channel_without_route);

    /* Go */
    RUN_TEST(sse_go_handler_emitter);
    RUN_TEST(sse_go_inline_emitter);
    RUN_TEST(sse_go_newclient_listener);
    RUN_TEST(sse_go_no_channel_without_route);
}
