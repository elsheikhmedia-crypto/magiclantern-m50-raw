// Decompiles Thumb functions at given addresses, or at the address stored in a
// pointer ("*0xADDR"). Creates the function first if Ghidra has none there.
// Args: list of "0xADDR" / "*0xADDR" / "name=0xADDR". Output: decompile_report.txt.
// @category M50
import java.io.File;
import java.io.FileWriter;
import java.io.PrintWriter;

import ghidra.app.cmd.disassemble.ArmDisassembleCommand;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        File out = new File(getSourceFile().getParentFile().getAbsolutePath(), "decompile_report.txt");
        try (PrintWriter w = new PrintWriter(new FileWriter(out, false))) {
            for (String arg : getScriptArgs()) {
                String label = arg, spec = arg;
                // "name:0xADDR" ('=' gets split by cmd.exe when passed through analyzeHeadless.bat)
                if (arg.contains(":")) { label = arg.split(":")[0]; spec = arg.split(":")[1]; }
                long a;
                if (spec.startsWith("*")) {
                    a = getInt(toAddr(Long.decode(spec.substring(1)))) & 0xffffffffL;
                } else {
                    a = Long.decode(spec);
                }
                Address addr = toAddr(a & ~1L);
                w.println("==================== " + label + " -> " + addr);
                Function f = getFunctionAt(addr);
                if (f == null) {
                    new ArmDisassembleCommand(addr, null, true).applyTo(currentProgram, monitor);
                    f = createFunction(addr, null);
                }
                if (f == null) { w.println("  (could not create function)"); continue; }
                DecompileResults res = dec.decompileFunction(f, 90, monitor);
                if (res == null || !res.decompileCompleted()) { w.println("  (decompile failed)"); continue; }
                w.println(res.getDecompiledFunction().getC());
            }
        }
        dec.dispose();
        println("DecompileAt: appended to " + out);
    }
}
