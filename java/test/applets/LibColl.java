import java.applet.Applet;
import java.awt.Graphics;
import java.util.Enumeration;
import java.util.Hashtable;
import java.util.NoSuchElementException;
import java.util.Vector;

/* java.util.Vector and java.util.Hashtable, which between them are the whole
   of the 1.0 collections library an applet ever reaches for. Enumeration order
   is part of what is compared: a Vector enumerates in index order, and this
   runtime's Hashtable enumerates in insertion order, which a real JVM does not
   promise - the keys are sorted before printing so the diff does not depend on
   a guarantee neither side makes. */
public class LibColl extends Applet {
    boolean done;

    public void paint(Graphics g) {
        if (!done) { done = true; run(); }
    }

    public static void main(String[] a) { run(); }

    static void p(String s) { System.out.println(s); }

    static String show(Vector v) {
        StringBuffer b = new StringBuffer("[");
        for (int i = 0; i < v.size(); i++) {
            if (i > 0) b.append(",");
            b.append((String) v.elementAt(i));
        }
        b.append("]");
        return b.toString();
    }

    /* Insertion sort over a Vector of Strings, so an unordered enumeration can
       still be printed in a fixed order. */
    static Vector sorted(Enumeration e) {
        Vector out = new Vector();
        while (e.hasMoreElements()) {
            String s = (String) e.nextElement();
            int i = 0;
            while (i < out.size() && ((String) out.elementAt(i)).compareTo(s) < 0)
                i++;
            out.insertElementAt(s, i);
        }
        return out;
    }

    static void run() {
        Vector v = new Vector();

        p("empty " + v.size() + " " + v.isEmpty());

        v.addElement("b");
        v.addElement("c");
        v.addElement("a");
        p("added " + show(v) + " " + v.size() + " " + v.isEmpty());

        p("at " + v.elementAt(0) + v.elementAt(2));
        p("first/last " + v.firstElement() + v.lastElement());
        p("contains " + v.contains("c") + v.contains("z"));
        p("indexOf " + v.indexOf("a") + " " + v.indexOf("z") + " "
          + v.indexOf("b", 1));

        v.insertElementAt("x", 1);
        p("insert " + show(v));
        v.insertElementAt("y", v.size());
        p("insert-end " + show(v));

        v.setElementAt("X", 1);
        p("set " + show(v));

        p("remove " + v.removeElement("X") + v.removeElement("nope") + " "
          + show(v));
        v.removeElementAt(0);
        p("removeAt " + show(v));

        try { v.elementAt(99); p("no throw"); }
        catch (ArrayIndexOutOfBoundsException e) { p("elementAt(99) throws"); }
        try { v.removeElementAt(-1); p("no throw"); }
        catch (ArrayIndexOutOfBoundsException e) { p("removeAt(-1) throws"); }

        StringBuffer eb = new StringBuffer();
        for (Enumeration e = v.elements(); e.hasMoreElements(); )
            eb.append((String) e.nextElement()).append(".");
        p("enum " + eb.toString());

        Enumeration ex = v.elements();
        while (ex.hasMoreElements()) ex.nextElement();
        try { ex.nextElement(); p("no throw"); }
        catch (NoSuchElementException e) { p("nextElement past end throws"); }

        String[] arr = new String[v.size()];
        v.copyInto(arr);
        p("copyInto " + arr.length + " " + arr[0]);

        Vector big = new Vector();
        for (int i = 0; i < 50; i++) big.addElement("n" + i);
        p("grown " + big.size() + " " + big.elementAt(0) + " "
          + big.elementAt(49) + " " + big.indexOf("n33"));

        v.removeAllElements();
        p("cleared " + v.size() + " " + v.isEmpty() + " " + show(v));

        Vector sz = new Vector(2);
        sz.addElement("p");
        sz.addElement("q");
        sz.addElement("r");
        p("presized " + show(sz));

        /* --- Hashtable --- */
        Hashtable h = new Hashtable();

        p("h-empty " + h.size() + " " + h.isEmpty());
        p("h-put " + h.put("one", "1") + " " + h.put("two", "2") + " "
          + h.put("one", "uno"));
        p("h-get " + h.get("one") + " " + h.get("two") + " " + h.get("nine"));
        p("h-size " + h.size() + " " + h.isEmpty());
        p("h-has " + h.containsKey("one") + h.containsKey("nine")
          + h.contains("2"));

        /* Keys are looked up by value, not identity: this is the case that
           actually gets used, an applet storing under a String it built. */
        String built = "o" + "ne";
        p("h-value-key " + h.get(built));

        h.put("three", "3");
        p("h-keys " + show(sorted(h.keys())));
        p("h-elements " + show(sorted(h.elements())));

        p("h-remove " + h.remove("two") + " " + h.remove("nope") + " "
          + h.size());
        p("h-keys2 " + show(sorted(h.keys())));

        Hashtable hi = new Hashtable();
        hi.put(new Integer(3), "three");
        p("h-intkey " + hi.get(new Integer(3)) + " " + hi.get(new Integer(4)));

        h.clear();
        p("h-clear " + h.size() + " " + h.isEmpty());
    }
}
