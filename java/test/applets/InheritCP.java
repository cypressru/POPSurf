/* Reduced from a 1999 applet whose classes inherit from one another.
 *
 * Calling a method the subclass inherits rather than declares, named at the
 * call site by the subclass. ps_jclass_find_method walks up to IBase and
 * returns IBase.origin, but do_invoke pushes the frame with the *resolved*
 * class IDerived - so IBase's code is interpreted against IDerived's
 * constant pool and every ldc, invoke and field index in it means something
 * else.
 *
 * Correct output is "origin=base". The runtime fails with
 * "unsupported ldc constant (tag 7)", the tag depending on what happens to
 * sit at that index in the wrong pool.
 */
import java.applet.Applet;
import java.awt.Graphics;

class IBase {
    String origin() { return "base"; }
}

class IDerived extends IBase {
    /* deliberately empty: origin() is inherited, not overridden */
    int unused;
}

public class InheritCP extends Applet {
    public void paint(Graphics g) {
        IDerived d = new IDerived();
        String   s = d.origin();

        g.drawString("origin " + s, 10, 20);
        System.out.println("origin=" + s);
    }
}
