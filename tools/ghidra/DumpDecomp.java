// Ghidra headless script: decompile one or more functions to C.
//
//   -postScript DumpDecomp.java <outDir> <addr|fn:Name|ref:addr> [...]
//
//   <addr>       decompile the function at that entry address
//   fn:Name      decompile the function with that (symbol) name
//   ref:addr     decompile every function that references that address
//
// Writes decomp_<function>.c into <outDir> and prints a one-line summary.
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;

public class DumpDecomp extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: DumpDecomp <outDir> <addr|fn:Name|ref:addr> [...]");
            return;
        }
        String outDir = args[0];
        new File(outDir).mkdirs();

        List<Address> targets = new ArrayList<>();
        for (int i = 1; i < args.length; i++) {
            String spec = args[i];
            if (spec.startsWith("ref:")) {
                Address a = toAddr(spec.substring(4));
                for (Reference r : getReferencesTo(a)) {
                    Function f = getFunctionContaining(r.getFromAddress());
                    if (f != null) {
                        targets.add(f.getEntryPoint());
                    }
                }
                println("refs to " + a + " -> " + targets.size() + " function(s)");
            } else if (spec.startsWith("fn:")) {
                Function f = getFunction(spec.substring(3));
                if (f == null) {
                    println("function not found: " + spec);
                } else {
                    targets.add(f.getEntryPoint());
                }
            } else {
                targets.add(toAddr(spec));
            }
        }

        DecompInterface di = new DecompInterface();
        di.setOptions(new DecompileOptions());
        di.openProgram(currentProgram);

        Set<String> done = new LinkedHashSet<>();
        for (Address t : targets) {
            if (!done.add(t.toString())) {
                continue;
            }
            Function f = getFunctionAt(t);
            if (f == null) {
                println("no function at " + t);
                continue;
            }
            DecompileResults res = di.decompileFunction(f, 120, monitor);
            String body;
            if (res != null && res.getDecompiledFunction() != null) {
                body = res.getDecompiledFunction().getC();
            } else {
                body = "// decompile failed: " + (res == null ? "null result" : res.getErrorMessage());
            }
            String name = f.getName();
            File out = new File(outDir, "decomp_" + name + ".c");
            try (PrintWriter pw = new PrintWriter(out)) {
                pw.println("// " + name + " @ " + f.getEntryPoint() + " (size " + f.getBody().getNumAddresses() + ")");
                pw.println(body);
            }
            println("wrote " + out.getName() + " (" + body.length() + " chars)");
        }
        di.dispose();
    }
}
