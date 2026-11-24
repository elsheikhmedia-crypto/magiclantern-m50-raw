// Args: search terms. Like FindM50Targets, plus pointer-to-string xrefs.
// For each search term, finds every null-terminated string in ROM0 containing it,
// lists the code that references that string, and decompiles the referencing
// function lines that mention it. Output: m50_targets_report.txt (next to this script).
// Used to locate the M50 1.1.0 equivalents of the EOS R raw_draft_v2 stubs.
// @category M50
import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;

public class FindStrings extends GhidraScript {
    static final String[] TERMS = {
        "SetEventFlag", "ClearEventFlag", "WaitForAnyEventFlag", "DeleteEventFlag", "CreateEventFlag",
        "LockEngineResources", "CreateResLockEntry", "PwrMng", "WakeSubChips", "SuspendSubChips",
        "MemToMem", "SetupEDMAC", "RegisterCBR", "ResetCBR", "ClearEdmacCBR", "StartEDmac",
        "ConnectReadEDmac", "lv_save_raw", "lv_set_mm", "AccumH", "get_fps_register",
    };

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        File out = new File(getSourceFile().getParentFile().getAbsolutePath(), "find_strings_report.txt");
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            for (String term : getScriptArgs()) {
                w.println("==================== " + term);
                Set<Address> starts = new LinkedHashSet<>();
                Address a = mem.getMinAddress();
                while (a != null && !monitor.isCancelled()) {
                    a = find(a, term.getBytes("US-ASCII"));
                    if (a == null) break;
                    starts.add(stringStart(mem, a));
                    a = a.add(1);
                }
                for (Address s : starts) {
                    String text = readCString(mem, s);
                    Reference[] refs = getReferencesTo(s);
                    w.println(String.format("  str %s \"%s\"  refs=%d", s, text, refs.length));
                    // pointers to the string (data tables), and who references them
                    byte[] le = new byte[4]; long v = s.getOffset();
                    for (int k = 0; k < 4; k++) le[k] = (byte) (v >> (8 * k));
                    Address pa = mem.getMinAddress(); int np = 0;
                    while (pa != null && np < 6 && !monitor.isCancelled()) {
                        pa = find(pa, le);
                        if (pa == null) break;
                        np++;
                        Reference[] prefs = getReferencesTo(pa);
                        w.println(String.format("    ptr @ %s  refs=%d", pa, prefs.length));
                        for (int k = 0; k < prefs.length && k < 4; k++) {
                            Function pf = getFunctionContaining(prefs[k].getFromAddress());
                            w.println(String.format("      from %s in %s", prefs[k].getFromAddress(),
                                pf == null ? "<no function>" : pf.getName() + " @ " + pf.getEntryPoint()));
                        }
                        pa = pa.add(1);
                    }
                    int shown = 0;
                    for (Reference r : refs) {
                        if (shown++ >= 6) { w.println("    ..."); break; }
                        Function f = getFunctionContaining(r.getFromAddress());
                        w.println(String.format("    from %s in %s", r.getFromAddress(),
                                f == null ? "<no function>" : f.getName() + " @ " + f.getEntryPoint()));
                        if (f == null) continue;
                        DecompileResults res = dec.decompileFunction(f, 30, monitor);
                        if (res == null || !res.decompileCompleted()) continue;
                        for (String line : res.getDecompiledFunction().getC().split("\n")) {
                            if (line.contains(text.length() > 40 ? text.substring(0, 40) : text)) {
                                w.println("      | " + line.trim());
                            }
                        }
                    }
                }
            }
        }
        dec.dispose();
        println("FindM50Targets: wrote " + out);
    }

    private Address stringStart(Memory mem, Address a) throws Exception {
        Address s = a;
        for (int i = 0; i < 128; i++) {
            Address p = s.subtract(1);
            byte b = mem.getByte(p);
            if (b < 0x20 || b > 0x7e) break;
            s = p;
        }
        return s;
    }

    private String readCString(Memory mem, Address s) throws Exception {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 160; i++) {
            byte b = mem.getByte(s.add(i));
            if (b == 0) break;
            sb.append((char) (b & 0xff));
        }
        return sb.toString();
    }
}
