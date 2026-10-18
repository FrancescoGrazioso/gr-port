// For each string argument: finds the defined string data containing it, lists its references and
// decompiles each referencing function into <out>.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import java.io.*;
import java.util.*;

public class StrRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(args[0]));
        DecompInterface d = new DecompInterface();
        d.openProgram(currentProgram);
        Set<Function> done = new HashSet<>();
        for (int i = 1; i < args.length; ++i) {
            String needle = args[i];
            DataIterator it = currentProgram.getListing().getDefinedData(true);
            while (it.hasNext()) {
                Data data = it.next();
                if (!(data.getValue() instanceof String)) continue;
                String s = (String) data.getValue();
                if (!s.contains(needle)) continue;
                out.println("// string \"" + s + "\" at " + data.getAddress());
                for (Reference r : getReferencesTo(data.getAddress())) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    out.println("//   ref from " + r.getFromAddress() + (f != null ? " in " + f.getName() : ""));
                    if (f != null && done.add(f) && done.size() < 40) {
                        DecompileResults res = d.decompileFunction(f, 120, monitor);
                        out.println(res.decompileCompleted() ? res.getDecompiledFunction().getC() : "// failed");
                    }
                }
            }
        }
        out.close();
    }
}
