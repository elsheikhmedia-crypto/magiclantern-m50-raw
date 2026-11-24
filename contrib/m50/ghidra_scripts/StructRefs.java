// Lists every function that references a RAM struct (default: Canon LV raw struct 0x12748),
// and prints the decompiled lines of each that touch it. Output: struct_refs_report.txt.
// Args: "base:0x12748" "size:0x100"
// @category M50
import java.io.File;
import java.io.PrintWriter;
import java.util.LinkedHashMap;
import java.util.Map;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;

public class StructRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        long base = 0x12748L, size = 0x100L;
        for (String a : getScriptArgs()) {
            if (a.startsWith("base:")) base = Long.decode(a.substring(5));
            if (a.startsWith("size:")) size = Long.decode(a.substring(5));
        }
        // 1) direct references Ghidra recorded
        Map<Function, String> funcs = new LinkedHashMap<>();
        for (long a = base; a < base + size; a += 4) {
            for (var r : getReferencesTo(toAddr(a))) {
                Function f = getFunctionContaining(r.getFromAddress());
                if (f != null) funcs.putIfAbsent(f, String.format("ref to 0x%x at %s", a, r.getFromAddress()));
            }
        }
        // 2) literal-pool words / immediates equal to base (movw/movt pairs often show as scalars)
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            for (int i = 0; i < ins.getNumOperands(); i++) {
                for (Object o : ins.getOpObjects(i)) {
                    if (o instanceof Scalar s && s.getUnsignedValue() == base) {
                        Function f = getFunctionContaining(ins.getAddress());
                        if (f != null) funcs.putIfAbsent(f, "scalar 0x" + Long.toHexString(base) + " at " + ins.getAddress());
                    }
                }
            }
        }
        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        File out = new File(getSourceFile().getParentFile().getAbsolutePath(), "struct_refs_report.txt");
        String tag = String.format("%x", base);
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.println("functions referencing 0x" + tag + ": " + funcs.size());
            for (var e : funcs.entrySet()) {
                Function f = e.getKey();
                w.println("==================== " + f.getName() + " @ " + f.getEntryPoint() + "  (" + e.getValue() + ")");
                DecompileResults res = dec.decompileFunction(f, 60, monitor);
                if (res == null || !res.decompileCompleted()) { w.println("  (decompile failed)"); continue; }
                String c = res.getDecompiledFunction().getC();
                // print whole function if short, else only lines with the struct / its DAT_ pointer
                String[] lines = c.split("\n");
                if (lines.length <= 60) { w.println(c); continue; }
                for (String l : lines)
                    if (l.contains("DAT_") || l.contains(tag) || l.contains("(") && l.contains("FUN_")) w.println("  | " + l.trim());
            }
        }
        dec.dispose();
        println("StructRefs: " + funcs.size() + " functions -> " + out);
    }
}
