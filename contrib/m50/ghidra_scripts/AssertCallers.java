// For assert/debug strings of the form "!IS_ERROR(Xxx(...))", decompiles every
// function that references the string and prints the lines just before it,
// so the call to Xxx (FUN_xxxxxxxx) can be read off. Also resolves the function
// pointers passed to the eventproc register calls for a few names.
// Output: assert_callers_report.txt next to this script.
// @category M50
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class AssertCallers extends GhidraScript {
    // string address -> what we expect it to reveal
    static final long[][] STRINGS = {
        {0xe01a2c90L}, {0xe01a2e0cL}, {0xe01a2fc0L}, {0xe01a32b4L}, {0xe06e3548L},   // DeleteEventFlag
        {0xe06e3518L}, {0xe127e4acL}, {0xe128be08L},                                // WaitForAnyEventFlag
        {0xe126bc9cL},                                                               // SetEventFlag
        {0xe01bf6e4L}, {0xe01bf904L},                                                // CreateEventFlagStrictly
        {0xe11c15b4L}, {0xe11c1718L},                                                // ClearEventFlag wrappers
        {0xe013ef54L}, {0xe013ef6cL},                                                // EldPwrMng Enable/Disable SubChip
        {0xe005af98L}, {0xe00dc658L}, {0xe080d278L},                                 // lv_save_raw / lv_set_mm
    };
    static final long[] DATA_PTRS = {0xe00dc654L, 0xe080d274L};                     // eventproc func ptrs

    @Override
    public void run() throws Exception {
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        File out = new File(getSourceFile().getParentFile().getAbsolutePath(), "assert_callers_report.txt");
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            for (long[] s : STRINGS) {
                Address sa = toAddr(s[0]);
                w.println("==================== string @ " + sa);
                for (Reference r : getReferencesTo(sa)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f == null) { w.println("  from " + r.getFromAddress() + " <no function>"); continue; }
                    w.println("  in " + f.getName() + " @ " + f.getEntryPoint());
                    DecompileResults res = dec.decompileFunction(f, 60, monitor);
                    if (res == null || !res.decompileCompleted()) { w.println("    (decompile failed)"); continue; }
                    String[] lines = res.getDecompiledFunction().getC().split("\n");
                    String tag = String.format("%x", s[0]);
                    for (int i = 0; i < lines.length; i++) {
                        if (!lines[i].contains(tag)) continue;
                        for (int k = Math.max(0, i - 5); k <= Math.min(lines.length - 1, i + 1); k++) {
                            w.println("    " + (k == i ? ">" : "|") + " " + lines[k].trim());
                        }
                        w.println("    ---");
                    }
                }
            }
            w.println("==================== eventproc function pointers");
            for (long p : DATA_PTRS) {
                Address pa = toAddr(p);
                long v = getInt(pa) & 0xffffffffL;
                Function f = getFunctionContaining(toAddr(v & ~1L));
                w.println(String.format("  [%s] = 0x%08x -> %s", pa, v, f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
            }
        }
        dec.dispose();
        println("AssertCallers: wrote " + out);
    }
}
