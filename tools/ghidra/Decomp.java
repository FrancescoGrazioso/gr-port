// Decompiles the functions containing the given addresses (script args, hex) into <out>.
// Usage: analyzeHeadless <proj> <name> -process eboot.elf -noanalysis -scriptPath <dir> -postScript Decomp.java <out> <addr>...
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.io.*;

public class Decomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(args[0]));
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        long base = currentProgram.getImageBase().getOffset();
        for (int i = 1; i < args.length; ++i) {
            long off = Long.parseUnsignedLong(args[i].replace("0x", ""), 16);
            Address a = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(base + off);
            Function f = getFunctionContaining(a);
            if (f == null) { out.println("// no function at " + a); continue; }
            out.println("// ===== " + args[i] + " in " + f.getName() + " @ " + f.getEntryPoint() + " (eboot+0x" + Long.toHexString(f.getEntryPoint().getOffset() - base) + ")");
            DecompileResults r = d.decompileFunction(f, 120, monitor);
            out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
        }
        out.close();
    }
}
