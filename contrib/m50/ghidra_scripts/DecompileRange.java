// Decompiles every function whose entry point lies in [start, end).
// Args: "0xSTART" "0xEND". Output: decompile_range_report.txt.
// @category M50
import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class DecompileRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        File out = new File(getSourceFile().getParentFile().getAbsolutePath(), "decompile_range_report.txt");
        try (PrintWriter w = new PrintWriter(new FileWriter(out, false))) {
            String[] a = getScriptArgs();
            for (int i = 0; i + 1 < a.length; i += 2) {
                Address s = toAddr(Long.decode(a[i])), e = toAddr(Long.decode(a[i + 1]));
                FunctionIterator it = currentProgram.getFunctionManager().getFunctions(s, true);
                while (it.hasNext()) {
                    Function f = it.next();
                    if (f.getEntryPoint().compareTo(e) >= 0) break;
                    w.println("==================== " + f.getEntryPoint());
                    DecompileResults r = dec.decompileFunction(f, 60, monitor);
                    w.println(r != null && r.decompileCompleted() ? r.getDecompiledFunction().getC() : "  (decompile failed)");
                }
            }
        }
        dec.dispose();
        println("DecompileRange: wrote " + out);
    }
}
