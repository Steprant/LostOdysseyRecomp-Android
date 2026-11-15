"""Steprant 2.1.1: tools/ppc_promotion_check.py on synthetic generated code."""
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import ppc_promotion_check as check  # noqa: E402

SOURCE = """\
PPC_FUNC_IMPL(__imp__sub_82000000) {
\tPPC_FUNC_PROLOGUE();
\tctx.r12.u64 = ctx.cr0.lt ? 0x80000000 : 0;
\tctx.r12.u64 |= ctx.cr2.eq ? 0x200000 : 0;
\tctx.r12.u64 = (ctx.cr6.lt << 7) | (ctx.cr6.gt << 6) | (ctx.cr6.eq << 5) | (ctx.cr6.so << 4);
\tctx.cr6.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);
\tif (ctx.cr6.eq) goto loc_82000010;
\tctx.xer.ca = ctx.r3.u32 < 4;
\tctx.r4.u64 = ctx.r4.u64 + ctx.xer.ca;
\tctx.cr0.so = ctx.xer.so;
loc_82000010:
\tctx.reserved.u32 = *(uint32_t*)(base + ctx.r5.u32);
\tif (ctx.reserved.u32 == 0) return;
\treturn;
}
PPC_FUNC_IMPL(__imp__sub_82000100) {
\tPPC_FUNC_PROLOGUE();
\tif (!ctx.cr6.eq) goto loc_82000110;
\tctx.r4.u64 = ctx.r4.u64 + ctx.xer.ca;
loc_82000110:
\tctx.cr0.eq = ctx.cr0.lt;
\tif (ctx.reserved.u32 == 0) return;
\treturn;
}
PPC_FUNC_IMPL(__imp__sub_82000200) {
\tgoto loc_82000220;
loc_82000210:
\tif (ctx.cr6.eq) return;
\treturn;
loc_82000220:
\tctx.cr2.compare<int32_t>(ctx.r3.s32, 1, ctx.xer);
\tsub_82000000(ctx, base);
\tctx.cr6.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);
\tif (ctx.cr2.gt) goto loc_82000210;
\tgoto loc_82000210;
}
PPC_FUNC_IMPL(__imp__sub_82000300) {
\tctx.cr6.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);
\tsub_82000000(ctx, base);
\tif (ctx.cr6.lt) return;
\treturn;
}
PPC_FUNC_IMPL(__imp__sub_82000400) {
\tswitch (ctx.r11.u64) {
\tcase 0:
\t\tgoto loc_82000410;
\tcase 1:
\t\tgoto loc_82000420;
\tdefault:
\t\t__builtin_unreachable();
\t}
loc_82000410:
\tctx.cr7.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);
\treturn;
loc_82000420:
\tif (ctx.cr7.eq) return;
\treturn;
}
PPC_FUNC_IMPL(__imp__sub_82000500) {
\tctx.cr6.compare(ctx.f1.f64, ctx.f2.f64);
loc_82000504:
\tif (ctx.cr6.eq) goto loc_82000510;
\tctx.cr6.compare<int32_t>(ctx.r4.s32, 0, ctx.xer);
\tgoto loc_82000504;
loc_82000510:
\treturn;
}
"""


def flagged_names(flagged):
    result = {}
    for label, entries in flagged.items():
        for name, sources in entries:
            result.setdefault(name, set()).add((label, sources))
    return result


class PromotionCheckTest(unittest.TestCase):
    def setUp(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "ppc_recomp.0.cpp"
            path.write_text(SOURCE, encoding="utf-8")
            self.functions, flagged = check.scan([path])
        self.names = flagged_names(flagged)

    def test_counts_every_function(self):
        self.assertEqual(self.functions, 6)

    def test_write_first_code_is_clean(self):
        # Compare before branch, carry set before use, mfcr saves, the SO
        # copy, a loop whose read follows a write on every path, and a read
        # that comes first in the text but after a write in control flow.
        for name in ("sub_82000000", "sub_82000200", "sub_82000500"):
            self.assertNotIn(name, self.names, name)

    def test_entry_reads(self):
        self.assertEqual(self.names["sub_82000100"], {
            ("cr6.eq", "entry"), ("cr0.lt", "entry"), ("xer.ca", "entry"), ("reserved", "entry")})

    def test_volatile_read_after_call(self):
        # cr6 is volatile: reading it after a call reads what the callee left.
        self.assertEqual(self.names["sub_82000300"], {("cr6.lt", "call")})

    def test_switch_cases_are_followed(self):
        self.assertEqual(self.names["sub_82000400"], {("cr7.eq", "entry")})


if __name__ == "__main__":
    unittest.main()
