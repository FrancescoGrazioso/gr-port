// Lists the functions whose instructions use any of the given scalar operands (hex), optionally
// decompiling them. Args: <out> <decompile 0|1> <scalar>...
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import java.io.*;
import java.util.*;

public class ScalarRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(a[0]));
        boolean dec = a[1].equals("1");
        Set<Long> wanted = new HashSet<>();
        for (int i = 2; i < a.length; ++i) wanted.add(Long.parseLong(a[i].replace("0x", ""), 16));
        Map<Function, Set<String>> hits = new LinkedHashMap<>();
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Instruction ins = it.next();
            for (int op = 0; op < ins.getNumOperands(); ++op)
                for (Object o : ins.getOpObjects(op))
                    if (o instanceof Scalar && wanted.contains(((Scalar) o).getUnsignedValue())) {
                        Function f = getFunctionContaining(ins.getAddress());
                        if (f != null) hits.computeIfAbsent(f, k -> new TreeSet<>()).add(ins.getAddress() + ": " + ins);
                    }
        }
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        for (Map.Entry<Function, Set<String>> e : hits.entrySet()) {
            out.println("// ===== " + e.getKey().getName() + " @ " + e.getKey().getEntryPoint());
            for (String s : e.getValue()) out.println("//   " + s);
            if (dec) {
                DecompileResults r = d.decompileFunction(e.getKey(), 120, monitor);
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed");
            }
        }
        out.close();
    }
}
