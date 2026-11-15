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
\tctx.cr6.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);
\tif (ctx.cr6.eq) goto loc_82000010;
\tctx.xer.ca = ctx.r3.u32 < 4;
\tctx.r4.u64 = ctx.r4.u64 + ctx.xer.ca;
\tctx.reserved.u32 = *(uint32_t*)(base + ctx.r5.u32);
\tif (ctx.reserved.u32 == 0) return;
}
PPC_FUNC_IMPL(__imp__sub_82000100) {
\tPPC_FUNC_PROLOGUE();
\tif (!ctx.cr6.eq) goto loc_82000110;
\tctx.r4.u64 = ctx.r4.u64 + ctx.xer.ca;
\tctx.cr0.eq = ctx.cr0.lt;
\tif (ctx.reserved.u32 == 0) return;
}
PPC_FUNC_IMPL(__imp__sub_82000200) {
\tctx.cr1.setFromMask(simde_mm_load_ps(ctx.v1.f32), 0xF);
\tif (ctx.cr1.lt) return;
\tctx.cr7.eq = (ctx.r12.u32 & 0x2) != 0;
\tif (ctx.cr7.eq) return;
}
"""


class PromotionCheckTest(unittest.TestCase):
    def test_reads_before_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "ppc_recomp.0.cpp"
            path.write_text(SOURCE, encoding="utf-8")
            functions, flagged = check.scan([path])
        self.assertEqual(functions, 3)
        names = {k: sorted({n for n, _, _ in v}) for k, v in flagged.items()}
        # Write-first code, mfcr saves and the compare's SO copy are fine.
        self.assertNotIn("sub_82000000", sum(names.values(), []))
        self.assertNotIn("sub_82000200", sum(names.values(), []))
        # A branch on cr6, an add-extended carry, a right-hand side read of cr0
        # and an stwcx-like use without lwarx all read what a caller left.
        self.assertEqual(names["cr6"], ["sub_82000100"])
        self.assertEqual(names["cr0"], ["sub_82000100"])
        self.assertEqual(names["xer.ca"], ["sub_82000100"])
        self.assertEqual(names["reserved"], ["sub_82000100"])


if __name__ == "__main__":
    unittest.main()
