// Args: hex addresses (literal pool entries). For each: references to it, the
// containing function, and its decompiled C. Output: refs_decomp_report.txt
// @category M50
import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashSet;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class RefsToDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        File out = new File(getSourceFile().getParentFile().getAbsolutePath(), "refs_decomp_report.txt");
        LinkedHashSet<Function> done = new LinkedHashSet<>();
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            for (String arg : getScriptArgs()) {
                Address a = toAddr(Long.parseLong(arg.replace("0x", ""), 16));
                w.println("==================== pool " + a);
                for (Reference r : getReferencesTo(a)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    w.println("  ref from " + r.getFromAddress() + " in " + (f == null ? "?" : f.getName() + " @ " + f.getEntryPoint()));
                    if (f == null || done.contains(f)) continue;
                    done.add(f);
                    DecompileResults res = dec.decompileFunction(f, 60, monitor);
                    if (res.decompileCompleted())
                        w.println(res.getDecompiledFunction().getC());
                }
            }
        }
    }
}
