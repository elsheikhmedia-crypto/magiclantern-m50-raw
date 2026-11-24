// Seeds analysis of the M50 1.1.0 ROM with the functions already known in
// platform/M50.110/stubs.S (exported to m50_stubs.txt as "0xADDR name [arm]").
// @category M50
import java.io.BufferedReader;
import java.io.FileReader;

import ghidra.app.cmd.disassemble.ArmDisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class SeedM50Stubs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String path = getScriptArgs().length > 0 ? getScriptArgs()[0]
                : "m50_stubs.txt";
        int seeded = 0, failed = 0;
        try (BufferedReader r = new BufferedReader(new FileReader(path))) {
            String line;
            while ((line = r.readLine()) != null) {
                String[] p = line.trim().split("\\s+");
                if (p.length < 2) continue;
                boolean thumb = !(p.length > 2 && p[2].equals("arm"));
                Address addr = toAddr(Long.decode(p[0]) & ~1L);
                if (!currentProgram.getMemory().contains(addr)) { failed++; continue; }
                new ArmDisassembleCommand(addr, null, thumb).applyTo(currentProgram, monitor);
                Function fn = getFunctionAt(addr);
                if (fn == null) fn = createFunction(addr, p[1]);
                if (fn != null) { fn.setName(p[1], SourceType.USER_DEFINED); seeded++; }
                else { createLabel(addr, p[1], true); failed++; }
            }
        }
        println("SeedM50Stubs: seeded=" + seeded + " failed=" + failed);

        // Slow decompiler-driven analyzers; FindM50Targets decompiles what it needs itself.
        setAnalysisOption(currentProgram, "Decompiler Parameter ID", "false");
        setAnalysisOption(currentProgram, "Decompiler Switch Analysis", "false");
    }
}
