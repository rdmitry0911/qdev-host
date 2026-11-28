/* Test fixture for selftest step 10f (D-QDEV-RELOAD-DLOPEN-NO-ROLLBACK): a shared
 * object with a SONAME (so the reload's unique-SONAME stamp accepts it) whose
 * dlopen(RTLD_NOW) fails with "undefined symbol" -- the shape of a libmlapi linked
 * against an older libair2spirv that exported SPIRV-Tools (apple-virgl
 * evidence/runs/main-v79-20261128 hpcaps: "undefined symbol: spvValidatorOption"). */
extern int uvg_selftest_unresolved_symbol(void);

int qmu_create_extended(void)
{
    return uvg_selftest_unresolved_symbol();
}
