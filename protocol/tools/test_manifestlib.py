# Copyright (c) 2026 Ekorau LLC
"""manifestlib contract tests (#243). The sample below is a miniature of
MANIFEST.cddl exercising every parser behavior the gates rely on: stacked
annotations, one-segment wildcards, literal-shadows-wildcard, and the
closed vocabularies."""
import pytest
from manifestlib import parse, ManifestError

SAMPLE = """\
sym-Health = #6.39("Health")
sym-up = #6.39("up")

;# path=obs/health  kind=cell  class=routine  dir=up
health = #6.27([sym-Health, { sym-up => uint }])

;# path=goal/config/<app>/<key>  kind=cell  class=routine  dir=down
;# path=obs/config/<app>/<key>  kind=cell  class=routine  dir=up
config-cell = scalar

;# path=diag/spans/ctl  kind=cell  class=routine  dir=up
span-ctl = #6.27([sym-Health, { sym-up => uint }])

;# path=diag/spans/<span>  kind=cell  class=routine  dir=up
latency-span = #6.27([sym-Health, { sym-up => uint }])

scalar = int / tstr / bool / #6.39(tstr)
"""


def test_parse_entries_and_stacked_annotations():
    m = parse(SAMPLE)
    assert len(m.entries) == 5  # config-cell carries TWO stacked annotations
    e = m.match("obs/health")
    assert e.rule == "health" and e.kind == "cell" and e.dir == "up"
    assert m.match("obs/config/a/k").rule == "config-cell"
    assert m.match("obs/config/a/k").dir == "up"
    assert m.match("goal/config/a/k").dir == "down"


def test_wildcard_matches_one_segment_only():
    m = parse(SAMPLE)
    assert m.match("goal/config/app1/key1").rule == "config-cell"
    assert m.match("goal/config/app1") is None
    assert m.match("goal/config/app1/key1/extra") is None


def test_literal_shadows_wildcard():
    m = parse(SAMPLE)
    assert m.match("diag/spans/ctl").rule == "span-ctl"
    assert m.match("diag/spans/pullSend").rule == "latency-span"


def test_unmatched_path_is_none():
    assert parse(SAMPLE).match("bogus/path") is None


def test_annotation_without_rule_raises():
    with pytest.raises(ManifestError):
        parse(';# path=a/b kind=cell class=routine dir=up\n\nsym-x = #6.39("x")\n')


def test_bad_vocab_raises():
    with pytest.raises(ManifestError):
        parse(";# path=a/b kind=blob class=routine dir=up\nr = tstr\n")


def test_syms_extracted():
    assert parse(SAMPLE).syms["sym-Health"] == "Health"


def test_multiline_rule_body_collected():
    text = """\
sym-A = #6.39("A")
sym-x = #6.39("x")

;# path=a/b  kind=cell  class=routine  dir=up
a = #6.27([sym-A, {
  sym-x => uint,
}])
"""
    m = parse(text)
    assert "sym-x => uint" in m.rules["a"]
    assert m.match("a/b").rule == "a"
