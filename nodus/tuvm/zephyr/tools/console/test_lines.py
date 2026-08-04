from lines import LineAssembler


def test_complete_line_emitted():
    assert LineAssembler().feed(b"hello\n") == ["hello"]


def test_partial_line_is_held_until_terminated():
    a = LineAssembler()
    assert a.feed(b"par") == []
    assert a.feed(b"tial\n") == ["partial"]


def test_nul_bytes_are_stripped():
    # the one hard lesson the untracked magtag capture.py already encoded
    assert LineAssembler().feed(b"he\x00llo\n") == ["hello"]


def test_crlf_is_normalised():
    assert LineAssembler().feed(b"hello\r\n") == ["hello"]


def test_multiple_lines_in_one_read():
    assert LineAssembler().feed(b"a\nb\nc\n") == ["a", "b", "c"]


def test_undecodable_bytes_do_not_raise():
    out = LineAssembler().feed(b"\xff\xfe ok\n")
    assert len(out) == 1 and "ok" in out[0]


def test_flush_emits_the_held_partial():
    a = LineAssembler()
    a.feed(b"dangling")
    assert a.flush() == ["dangling"]
    assert a.flush() == []


def test_empty_lines_survive():
    # a blank line in a fault dump is evidence of spacing, not noise
    assert LineAssembler().feed(b"a\n\nb\n") == ["a", "", "b"]
