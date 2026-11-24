// Decompiles the function CONTAINING each given address (Thumb). Also lists its callers.
// Args: optional first "out=NAME", then "label:0xADDR" ... Output in ghidra_scripts/NAME.
// @category M50
import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.LinkedHashSet;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class DecompContaining extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        String[] args = getScriptArgs();
        String name = "decomp_containing.txt";
        int start = 0;
        if (args.length > 0 && args[0].startsWith("out")) { name = args[0].substring(4); start = 1; }
        File out = new File(getSourceFile().getParentFile().getAbsolutePath(), name);
        try (PrintWriter w = new PrintWriter(new FileWriter(out, false))) {
            for (int i = start; i < args.length; i++) {
                String arg = args[i], label = arg, spec = arg;
                if (arg.contains(":")) { label = arg.split(":")[0]; spec = arg.split(":")[1]; }
                Address addr = toAddr(Long.decode(spec) & ~1L);
                Function f = getFunctionContaining(addr);
                w.println("==================== " + label + " @ " + addr + " -> " + (f == null ? "NONE" : f.getEntryPoint()));
                if (f == null) continue;
                LinkedHashSet<String> callers = new LinkedHashSet<>();
                for (Reference r : getReferencesTo(f.getEntryPoint())) {
                    Function c = getFunctionContaining(r.getFromAddress());
                    callers.add(r.getFromAddress() + "(" + (c == null ? "?" : c.getEntryPoint().toString()) + ")");
                }
                w.println("callers: " + callers);
                DecompileResults res = dec.decompileFunction(f, 120, monitor);
                if (res == null || !res.decompileCompleted()) { w.println("  (decompile failed)"); continue; }
                w.println(res.getDecompiledFunction().getC());
            }
        }
        dec.dispose();
        println("DecompContaining: wrote " + out);
    }
}
