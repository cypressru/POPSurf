import java.applet.Applet;
import java.awt.Graphics;

/* A second class that reaches java.util before the applet itself does.

   The library classes are put into the class table when a constructor first
   reaches one implemented in C, which for an applet is the super() call at the
   top of its own constructor. A helper class loaded later, whose first act is
   a static call into java.lang, is the case where that could be too late - it
   is loaded on demand, part-way through a method, and nothing has constructed
   an instance of it. This is here so that stays true. */
public class LibTwo extends Applet {
    boolean done;

    public void paint(Graphics g) {
        if (!done) { done = true; run(); }
    }

    public static void main(String[] a) { run(); }

    static void p(String s) { System.out.println(s); }

    static void run() {
        p("sum " + LibTwoHelp.total("3,4,5"));
        p("names " + LibTwoHelp.names("a b c"));
        p("bad " + LibTwoHelp.total("3,x,5"));
    }
}
