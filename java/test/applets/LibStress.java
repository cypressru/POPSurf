import java.applet.Applet;
import java.awt.Graphics;
import java.util.Enumeration;
import java.util.Hashtable;
import java.util.Random;
import java.util.StringTokenizer;
import java.util.Vector;

/* Allocation pressure, so the collector runs while the containers are being
   used rather than only after.

   This is the test that would have caught the thing worth catching. A native
   call's arguments sit above the operand stack pointer the collector scans, so
   an object that exists only as an argument - `v.addElement(new String(...))`
   - is not a root, and the array growth inside addElement allocates. Get that
   wrong and the element is freed while the Vector still points at it, which on
   a console with no memory protection is a crash minutes later in a function
   that did nothing wrong.

   Everything below allocates far more than the collector's threshold and then
   reads back what it stored, under a sanitizer on a host and against a real
   JVM for the values. */
public class LibStress extends Applet {
    boolean done;

    public void paint(Graphics g) {
        if (!done) { done = true; run(); }
    }

    public static void main(String[] a) { run(); }

    static void p(String s) { System.out.println(s); }

    static void run() {
        Vector v = new Vector();
        int i;

        /* Each element is a String that exists nowhere but on the operand
           stack when addElement is entered. */
        for (i = 0; i < 2000; i++)
            v.addElement("item-" + i + "-" + (i * 7));

        p("size " + v.size());
        p("head " + v.elementAt(0) + " " + v.elementAt(1));
        p("tail " + v.elementAt(1998) + " " + v.elementAt(1999));

        /* Read every element back and check it against what it should be.
           A freed element shows up here as a wrong string or as nothing. */
        int bad = 0;
        for (i = 0; i < v.size(); i++) {
            String want = "item-" + i + "-" + (i * 7);
            if (!want.equals((String) v.elementAt(i))) bad++;
        }
        p("intact " + bad);

        p("find " + v.indexOf("item-1234-8638") + " "
          + v.contains("item-0-0") + " " + v.contains("nope"));

        /* Removing from the front moves every element, repeatedly, while more
           allocation happens around it. */
        for (i = 0; i < 1000; i++) v.removeElementAt(0);
        p("after-remove " + v.size() + " " + v.elementAt(0));

        Hashtable h = new Hashtable();
        for (i = 0; i < 400; i++)
            h.put("k" + i, "v" + (i * 3));
        p("h-size " + h.size() + " " + h.get("k399") + " " + h.get("k0"));

        int hbad = 0;
        for (i = 0; i < 400; i++) {
            if (!("v" + (i * 3)).equals((String) h.get("k" + i))) hbad++;
        }
        p("h-intact " + hbad);

        int seen = 0;
        for (Enumeration e = h.keys(); e.hasMoreElements(); ) {
            String k = (String) e.nextElement();
            if (h.get(k) != null) seen++;
        }
        p("h-enum " + seen);

        /* Strings built and thrown away by the thousand, so the collector runs
           between the tokenizer's own allocations. */
        StringBuffer sb = new StringBuffer();
        for (i = 0; i < 300; i++) sb.append(i).append(",");
        StringTokenizer t = new StringTokenizer(sb.toString(), ",");
        int count = 0, sum = 0;
        while (t.hasMoreTokens()) {
            sum += Integer.parseInt(t.nextToken());
            count++;
        }
        p("tok " + count + " " + sum);

        Random r = new Random(5L);
        Vector nums = new Vector();
        for (i = 0; i < 500; i++) nums.addElement(String.valueOf(r.nextInt(1000)));
        p("rand " + nums.size() + " " + nums.elementAt(0) + " "
          + nums.elementAt(499));

        /* And the whole lot dropped, so the next collection has real work. */
        v.removeAllElements();
        h.clear();
        p("done " + v.size() + " " + h.size());
    }
}
