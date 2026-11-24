// For each target address, lists every function that calls it (or takes its address)
// and decompiles each caller. Args: "name:0xADDR". Output: callers_report.txt.
// @category M50
import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class CallersOf extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        File out = new File(getSourceFile().getParentFile().getAbsolutePath(), "callers_report.txt");
        try (PrintWriter w = new PrintWriter(new FileWriter(out, false))) {
            for (String arg : getScriptArgs()) {
                String label = arg.contains(":") ? arg.split(":")[0] : arg;
                Address t = toAddr(Long.decode(arg.contains(":") ? arg.split(":")[1] : arg) & ~1L);
                w.println("#################### callers of " + label + " " + t);
                Set<Function> callers = new LinkedHashSet<>();
                for (Address x : new Address[] { t, t.add(1) }) {
                    for (Reference r : getReferencesTo(x)) {
                        Function f = getFunctionContaining(r.getFromAddress());
                        w.println("  ref from " + r.getFromAddress() + " (" + r.getReferenceType() + ") in "
                                + (f == null ? "<no function>" : f.getEntryPoint()));
                        if (f != null) callers.add(f);
                    }
                }
                for (Function f : callers) {
                    w.println("==================== " + f.getEntryPoint());
                    DecompileResults r = dec.decompileFunction(f, 60, monitor);
                    w.println(r != null && r.decompileCompleted() ? r.getDecompiledFunction().getC() : "  (decompile failed)");
                }
            }
        }
        dec.dispose();
        println("CallersOf: wrote " + out);
    }
}
