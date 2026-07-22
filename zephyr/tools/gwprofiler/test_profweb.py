from profweb import render_span_table

def test_render_lists_each_slot_row():
    cell = {"_tag": "SpanCapture", "n": 2, "ms": 1000, "slots": [
        {"_tag": "LatencySpan", "span": "acquire", "n": 2, "fails": 0,
         "minUs": 100, "maxUs": 200}]}
    html = render_span_table(cell)
    assert "acquire" in html and "<td" in html and "100" in html and "200" in html

def test_render_handles_no_capture():
    html = render_span_table(None)
    assert "No capture" in html
