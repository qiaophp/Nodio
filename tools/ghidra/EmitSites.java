// Ghidra headless script: enumerate every call site of the official record
// emitter FUN_004f5b20(kind, dest) and recover the record constant that is
// pushed as the first argument.
//
//   -postScript EmitSites.java <outDir> <emitterAddr>
//
// Each hit prints: call site address, containing function, and the recovered
// 32-bit record word (opcode = word & 0x3f).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.Reference;

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeMap;

public class EmitSites extends GhidraScript {

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: EmitSites <outDir> <emitterAddr>");
            return;
        }
        String outDir = args[0];
        Address emitter = toAddr(args[1]);
        new File(outDir).mkdirs();
        PrintWriter pw = new PrintWriter(new File(outDir, "emitsites.txt"));

        TreeMap<Integer, List<String>> byKind = new TreeMap<>();
        int total = 0;

        for (Reference r : getReferencesTo(emitter)) {
            if (r.getReferenceType() != RefType.UNCONDITIONAL_CALL
                && r.getReferenceType() != RefType.CONDITIONAL_CALL) {
                continue;
            }
            total++;
            Address call = r.getFromAddress();
            Long word = findPushedImm(call);
            Function f = getFunctionContaining(call);
            String fn = (f == null) ? "<none>" : f.getName();
            String line;
            if (word == null) {
                line = "  " + call + "  in " + fn + "  kind=? (dynamic)";
            } else {
                int kind = (int) (word & 0x3f);
                line = String.format("  %s  in %-18s word=0x%08X  kind=0x%02X (%d)",
                    call, fn, word, kind, kind);
                List<String> lst = byKind.get(kind);
                if (lst == null) {
                    lst = new ArrayList<>();
                    byKind.put(kind, lst);
                }
                lst.add(fn);
            }
            pw.println(line);
            println(line);
        }

        pw.println();
        pw.println("// total call sites: " + total);
        pw.println("// kind -> emitting functions");
        for (java.util.Map.Entry<Integer, List<String>> e : byKind.entrySet()) {
            java.util.TreeSet<String> uniq = new java.util.TreeSet<>(e.getValue());
            pw.println(String.format("kind 0x%02X (%d): %d site(s), funcs=%s",
                e.getKey(), e.getKey(), e.getValue().size(), uniq));
        }
        pw.close();
        println("// call sites: " + total + ", distinct kinds: " + byKind.size());
    }

    // walk back from the call looking for the immediate that becomes arg1
    private Long findPushedImm(Address call) {
        Instruction ins = getInstructionBefore(call);
        for (int step = 0; step < 8 && ins != null; step++) {
            String mn = ins.getMnemonicString();
            if (mn != null && (mn.equalsIgnoreCase("push") || mn.equalsIgnoreCase("PUSH"))) {
                for (int i = 0; i < ins.getNumOperands(); i++) {
                    Object[] ops = ins.getOpObjects(i);
                    for (Object op : ops) {
                        if (op instanceof Scalar) {
                            return ((Scalar) op).getUnsignedValue();
                        }
                    }
                }
            }
            if (mn != null && (mn.equalsIgnoreCase("mov") || mn.equalsIgnoreCase("MOV"))) {
                for (int i = 0; i < ins.getNumOperands(); i++) {
                    Object[] ops = ins.getOpObjects(i);
                    for (Object op : ops) {
                        if (op instanceof Scalar) {
                            return ((Scalar) op).getUnsignedValue();
                        }
                    }
                }
            }
            ins = ins.getPrevious();
        }
        return null;
    }
}
