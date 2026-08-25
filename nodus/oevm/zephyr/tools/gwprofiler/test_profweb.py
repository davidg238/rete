from profweb import render_span_table


def test_render_lists_each_sibling_row():
    # tuvm#107: the rows arrive as SIBLING CELLS keyed by span name, not nested in
    # the summary — 8 x 140 B in one value was 4.4x the 256 B cap.
    summary = {"_tag": "SpanCapture", "n": 2, "ms": 1000}
    rows = {"acquire": {"_tag": "LatencySpan", "span": "acquire", "n": 2,
                        "fails": 0, "minUs": 100, "maxUs": 200}}
    html = render_span_table(summary, rows)
    assert "acquire" in html and "<td" in html and "100" in html and "200" in html


def test_render_handles_no_capture():
    html = render_span_table(None, {})
    assert "No capture" in html


def test_slot_with_no_samples_is_not_rendered_as_zero_microseconds():
    # A slot that took no samples is not "instantaneous". The em-dash says so.
    summary = {"_tag": "SpanCapture", "n": 0, "ms": 1000}
    rows = {"acquire": {"_tag": "LatencySpan", "span": "acquire", "n": 0,
                        "fails": 0, "minUs": 0, "maxUs": 0}}
    html = render_span_table(summary, rows)
    assert "&mdash;" in html
