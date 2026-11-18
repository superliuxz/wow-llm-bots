"""Tests for wlb_gen.py's RULES.md reply-form check (`tools/wlb check rules` runs them).

Usage: python3 -I -B tools/grammar/wlb_gen_test.py
"""

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import wlb_gen  # noqa: E402

WLB = """
env E { whisperers: names; }
int Option 1..9
text Chat max 80 no "\\n"
name Player from whisperers
lines Reply {
  do: option:Option "<an option>";
  say?: say:Chat "<optional: a line>";
  tell? <to:Player "Name">: tell:Chat "<optional: a whisper>";
  secret?: secret:Chat;
}
"""

FORM = "do: <an option>\nsay: <optional: a line>\ntell <Name>: <optional: a whisper>"
RULES = ("Rules.\n\nReply in exactly this form (omit `say:` and `tell` unless talking):\n"
         f"{FORM}\n\nExample:\ndo: 1\n")


class RulesTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.wlb = pathlib.Path(self.dir.name) / "reply.wlb"
        self.wlb.write_text(WLB)
        self.rules = pathlib.Path(self.dir.name) / "RULES.md"

    def check(self, text, fix=False):
        self.rules.write_text(text)
        return wlb_gen.check_rules(self.wlb, self.rules, fix)

    def test_form_is_documented_lines_in_order_with_key_labels(self):
        form, optional = wlb_gen.reply_form(wlb_gen.read_wlb(self.wlb)[0])
        self.assertEqual("\n".join(form), FORM)  # `secret` has no description.
        self.assertEqual(optional, ["say", "tell"])

    def test_up_to_date_passes(self):
        self.assertEqual(self.check(RULES), 0)

    def test_stale_fails_and_fix_rewrites_only_the_form(self):
        stale = RULES.replace("a line>", "one line>")
        self.assertEqual(self.check(stale), 1)
        self.assertEqual(self.rules.read_text(), stale)
        self.assertEqual(self.check(stale, fix=True), 0)
        self.assertEqual(self.rules.read_text(), RULES)

    def test_missing_anchors_fail_loudly(self):
        for text in (RULES.replace("Reply in exactly", "Answer in"),
                     RULES.replace("\n\nExample:", "\n\nFor example:")):
            with self.assertRaises(wlb_gen.WlbError):
                self.check(text, fix=True)

    def test_an_optional_key_the_intro_does_not_name_fails(self):
        with self.assertRaisesRegex(wlb_gen.WlbError, "tell"):
            self.check(RULES.replace("`tell`", "tell"))


if __name__ == "__main__":
    unittest.main()
