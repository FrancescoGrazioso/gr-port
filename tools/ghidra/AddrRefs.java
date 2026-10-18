// Lists references to the given addresses (hex, eboot offsets) with the containing function.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import java.io.*;

public class AddrRefs extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        PrintWriter out = new PrintWriter(new FileWriter(a[0]));
        for (int i = 1; i < a.length; ++i) {
            Address ad = toAddr(Long.parseUnsignedLong(a[i].replace("0x", ""), 16));
            out.println("// refs to " + ad);
            for (Reference r : getReferencesTo(ad)) {
                Function f = getFunctionContaining(r.getFromAddress());
                out.println("//   " + r.getFromAddress() + " " + r.getReferenceType() + (f != null ? " in " + f.getName() + " @ " + f.getEntryPoint() : ""));
            }
        }
        out.close();
    }
}
