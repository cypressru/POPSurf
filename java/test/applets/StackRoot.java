/* Reduced case: an operand-stack slot is not a GC root while its own method
 * is running.
 *
 * ps_jvm.c's run() caches the top frame's stack pointer in a C local and
 * writes it back to fr->sp only on a call, a return or a suspension. Nothing
 * else updates it, so for the whole body of a method fr->sp says whatever it
 * said at the last frame push - and mark_roots scans exactly
 * fr->stack[0 .. fr->sp). Every operand pushed since then is invisible to the
 * collector.
 *
 * That matters at any allocation the interpreter makes with operands live:
 * ldc of a string constant, new, and every ps_jre_call that builds an object.
 * The shape below is the common one - a string concatenation, whose partially
 * built StringBuilder sits on the operand stack across an append that
 * allocates.
 *
 * Nothing here touches geometry or anything else recent; it reproduces on the
 * runtime exactly as it stood before java/ps_jgeom.c existed. Under a
 * sanitised host build it is a heap-use-after-free in ps_jre_call's append;
 * unsanitised it corrupts the heap and dies inside malloc, which on a console
 * with no memory protection takes the browser with it.
 *
 * The real JDK prints "total 85130". This prints nothing.
 */
import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;

public class StackRoot extends Applet {
    public static void main(String[] a) { work(); }

    static void work() {
        int n = 0;

        for (int i = 0; i < 4000; i++) {
            Color  c = new Color(i & 255, 0, 0);
            String s = "value " + i + " red " + c.getRed() + " end";

            n += s.length();
        }
        System.out.println("total " + n);
    }

    public void paint(Graphics g) {
        work();
        g.drawString("StackRoot", 10, 20);
    }
}
