// Ghidra headless script: list every reference TO an address.
//
//   -postScript ListRefs.java <outDir> <addr> [...]
//
// Prints, per referenced address: the referencing address and the name of
// the function that contains it. Used to locate the callers of the official
// aardio.exe opcode decoder / node evaluator.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.File;
import java.io.PrintWriter;

public class ListRefs extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: ListRefs <outDir> <addr> [...]");
            return;
        }
        String outDir = args[0];
        new File(outDir).mkdirs();
        PrintWriter pw = new PrintWriter(new File(outDir, "refs.txt"));

        for (int i = 1; i < args.length; i++) {
            Address a = toAddr(args[i]);
            println("=== refs to " + a + " ===");
            pw.println("=== refs to " + a + " ===");
            for (Reference r : getReferencesTo(a)) {
                Function f = getFunctionContaining(r.getFromAddress());
                String fn = (f == null) ? "<none>" : f.getName();
                String line = "  from " + r.getFromAddress() + "  in " + fn +
                    "   (type " + r.getReferenceType() + ")";
                println(line);
                pw.println(line);
            }
        }
        pw.close();
    }
}
