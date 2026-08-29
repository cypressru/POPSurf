/* Every java.lang.Math entry an applet of this period reaches for.
 *
 * Seven applets in one author's block died on asin and atan after getting
 * four layers deep, having already drawn part of a frame. The transcendentals
 * are one line each; what is worth testing is that the two-double calls read
 * argument slot 2 rather than slot 1 - doubles occupy two slots, and reading
 * the padding computes silently with zero.
 */
import java.applet.Applet;
import java.awt.Graphics;

public class MathAll extends Applet {

    static String check() {
        StringBuffer sb = new StringBuffer();

        sb.append("sin=" + Math.sin(0.5) + "\n");
        sb.append("cos=" + Math.cos(0.5) + "\n");
        sb.append("tan=" + Math.tan(0.5) + "\n");
        sb.append("asin=" + Math.asin(0.5) + "\n");
        sb.append("acos=" + Math.acos(0.5) + "\n");
        sb.append("atan=" + Math.atan(0.5) + "\n");
        sb.append("atan2=" + Math.atan2(1.0, 2.0) + "\n");
        sb.append("log=" + Math.log(10.0) + "\n");
        sb.append("exp=" + Math.exp(1.0) + "\n");
        sb.append("sqrt=" + Math.sqrt(2.0) + "\n");
        sb.append("pow=" + Math.pow(2.0, 10.0) + "\n");
        sb.append("rem=" + Math.IEEEremainder(5.0, 3.0) + "\n");
        sb.append("rint=" + Math.rint(2.5) + " " + Math.rint(3.5) + "\n");
        sb.append("floor=" + Math.floor(-2.5) + " ceil=" + Math.ceil(-2.5)
                  + "\n");
        sb.append("round=" + Math.round(2.5) + " " + Math.round(-2.5) + "\n");
        sb.append("rad=" + Math.toRadians(180.0) + "\n");
        sb.append("deg=" + Math.toDegrees(Math.PI) + "\n");

        /* The slot-index cases: second argument of a (DD) or (JJ) call. */
        sb.append("dmin=" + Math.min(3.5, 1.25) + " dmax="
                  + Math.max(3.5, 1.25) + "\n");
        sb.append("lmin=" + Math.min(7L, 3L) + " lmax=" + Math.max(7L, 3L)
                  + "\n");
        sb.append("labs=" + Math.abs(-9000000000L) + "\n");
        sb.append("fmin=" + Math.min(1.5f, 2.5f) + " fmax="
                  + Math.max(1.5f, 2.5f) + "\n");
        sb.append("iabs=" + Math.abs(-7) + " dabs=" + Math.abs(-7.5) + "\n");

        return sb.toString();
    }

    public static void main(String[] args) { System.out.print(check()); }
    public void init() { System.out.print(check()); }
    public void paint(Graphics g) { g.drawString("math", 10, 20); }
}
