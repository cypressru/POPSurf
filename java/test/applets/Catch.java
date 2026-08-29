import java.applet.Applet;
import java.awt.Color;
import java.awt.Graphics;

/* Every throw path the VM can produce, each caught and reported as a green
   tick or a red cross. If any row is red the exception did not reach its
   handler; if the applet dies, unwinding is broken. */
public class Catch extends Applet {
    String[] label = new String[6];
    boolean[] ok = new boolean[6];

    public void paint(Graphics g) {
        int n = 0;
        int[] a = new int[3];
        int[] nul = null;

        // 1. explicit athrow, caught by exact type
        label[n] = "throw + catch exact";
        try { throw new RuntimeException(); }
        catch (RuntimeException e) { ok[n] = true; }
        catch (Exception e) { }
        n++;

        // 2. athrow caught by a supertype
        label[n] = "catch supertype";
        try { throw new RuntimeException(); }
        catch (Exception e) { ok[n] = true; }
        n++;

        // 3. divide by zero -> ArithmeticException
        label[n] = "divide by zero";
        try { int z = 0; int q = 7 / z; a[0] = q; }
        catch (ArithmeticException e) { ok[n] = true; }
        n++;

        // 4. array index -> ArrayIndexOutOfBoundsException
        label[n] = "array index";
        try { a[5] = 1; }
        catch (ArrayIndexOutOfBoundsException e) { ok[n] = true; }
        n++;

        // 5. null deref -> NullPointerException
        label[n] = "null array";
        try { int v = nul[0]; a[0] = v; }
        catch (NullPointerException e) { ok[n] = true; }
        n++;

        // 6. unwinding out through a nested call
        label[n] = "unwind through calls";
        try { deep(3); }
        catch (RuntimeException e) { ok[n] = true; }
        n++;

        g.setColor(Color.white);
        g.fillRect(0, 0, 300, 200);
        g.setColor(new Color(20, 20, 30));
        g.drawString("exceptions", 12, 20);

        for (int i = 0; i < n; i++) {
            int y = 42 + i * 24;
            if (ok[i]) {
                g.setColor(new Color(40, 150, 60));
                g.drawLine(14, y - 5, 18, y - 1);
                g.drawLine(18, y - 1, 26, y - 12);
            } else {
                g.setColor(new Color(200, 50, 40));
                g.drawLine(14, y - 12, 26, y - 1);
                g.drawLine(26, y - 12, 14, y - 1);
            }
            g.setColor(new Color(20, 20, 30));
            g.drawString(label[i], 38, y);
        }
    }

    void deep(int d) {
        if (d == 0) throw new RuntimeException();
        deep(d - 1);
    }
}
