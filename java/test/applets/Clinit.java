/* Static initialisers, NaN comparison, and MouseEvent.getPoint.
 *
 * All three were silent. <clinit> was never run at all, so a static table was
 * still null when paint() read it - and period applets keep their lookup
 * tables, sprite offsets and colour ramps in exactly that shape. NaN compared
 * equal to everything, so the standard `x != x` test for it was always false.
 */
import java.applet.Applet;
import java.awt.Graphics;

public class Clinit extends Applet {

    /* The shape that was broken: an array built by <clinit>, not <init>. */
    static final int[] TABLE = { 3, 1, 4, 1, 5, 9, 2, 6 };
    static final String NAME;
    static int computed;

    /* An explicit static block, which is the same method by another spelling. */
    static {
        NAME = "built";
        for(int i = 0; i < TABLE.length; i++)
            computed += TABLE[i];
    }

    static String check() {
        StringBuffer sb = new StringBuffer();

        sb.append("table=" + (TABLE == null ? "null" : "len" + TABLE.length)
                  + "\n");
        sb.append("first=" + TABLE[0] + " last=" + TABLE[TABLE.length - 1]
                  + "\n");
        sb.append("name=" + NAME + "\n");
        sb.append("sum=" + computed + "\n");

        double nan = Math.sqrt(-1.0);
        sb.append("nan!=nan=" + (nan != nan) + "\n");
        sb.append("nan<1=" + (nan < 1.0) + " nan>1=" + (nan > 1.0) + "\n");
        float fnan = (float)nan;
        sb.append("fnan!=fnan=" + (fnan != fnan) + "\n");
        sb.append("log10=" + Math.log(10.0) + "\n");

        return sb.toString();
    }

    public static void main(String[] args) { System.out.print(check()); }
    public void init() { System.out.print(check()); }

    public void paint(Graphics g) {
        g.drawString("clinit", 10, 20);
    }
}
