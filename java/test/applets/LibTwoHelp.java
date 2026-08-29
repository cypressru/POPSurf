import java.util.StringTokenizer;
import java.util.Vector;

/* Static methods only, so nothing constructs one of these. Loaded when
   LibTwo's paint calls into it, which is after the applet exists but before
   the applet has touched java.util itself. */
public class LibTwoHelp {
    static int total(String csv) {
        StringTokenizer t = new StringTokenizer(csv, ",");
        int sum = 0;

        while (t.hasMoreTokens()) {
            try { sum += Integer.parseInt(t.nextToken()); }
            catch (NumberFormatException e) { sum -= 1; }
        }
        return sum;
    }

    static String names(String s) {
        Vector v = new Vector();
        StringTokenizer t = new StringTokenizer(s);
        StringBuffer b = new StringBuffer();

        while (t.hasMoreTokens()) v.addElement(t.nextToken().toUpperCase());
        for (int i = 0; i < v.size(); i++) b.append((String) v.elementAt(i));
        return b.toString() + ":" + v.size();
    }
}
