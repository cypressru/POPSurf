import java.applet.Applet;
import java.awt.Graphics;

/* The primitive wrappers. The parse paths matter most: an applet validates a
   <param> value by handing it to Integer.parseInt inside a try, so what it
   accepts and exactly when it throws is behaviour applets depend on. */
public class LibNum extends Applet {
    boolean done;

    public void paint(Graphics g) {
        if (!done) { done = true; run(); }
    }

    public static void main(String[] a) { run(); }

    static void p(String s) { System.out.println(s); }

    static void pi(String v) {
        try { p("parseInt [" + v + "] = " + Integer.parseInt(v)); }
        catch (NumberFormatException e) { p("parseInt [" + v + "] throws"); }
    }

    static void run() {
        p("limits " + Integer.MIN_VALUE + " " + Integer.MAX_VALUE);
        p("limitsL " + Long.MIN_VALUE + " " + Long.MAX_VALUE);
        p("limitsC " + (int) Character.MIN_VALUE + " "
          + (int) Character.MAX_VALUE);

        pi("0");
        pi("42");
        pi("-42");
        pi("+42");
        pi("2147483647");
        pi("-2147483648");
        pi("2147483648");
        pi("");
        pi("   7");
        pi("7 ");
        pi("7.0");
        pi("abc");
        pi("-");
        pi("0x10");

        p("radix " + Integer.parseInt("ff", 16) + " "
          + Integer.parseInt("-101", 2) + " " + Integer.parseInt("z", 36));
        try { p("bad " + Integer.parseInt("2", 2)); }
        catch (NumberFormatException e) { p("parseInt(2,2) throws"); }

        p("toString " + Integer.toString(255) + " " + Integer.toString(255, 16)
          + " " + Integer.toString(-255, 16) + " " + Integer.toString(0, 2));
        p("hex " + Integer.toHexString(255) + " " + Integer.toHexString(-1)
          + " " + Integer.toOctalString(8) + " "
          + Integer.toBinaryString(5));

        Integer bi = new Integer(7);
        p("box " + bi.intValue() + " " + bi.doubleValue() + " "
          + bi.longValue() + " " + bi.floatValue());
        p("boxstr " + bi.toString() + " " + Integer.valueOf(9).intValue()
          + " " + Integer.valueOf("11").intValue());
        p("boxeq " + bi.equals(new Integer(7)) + bi.equals(new Integer(8)));
        p("boxhash " + bi.hashCode());

        p("longparse " + Long.parseLong("9007199254740993") + " "
          + Long.parseLong("-5") + " " + Long.toString(255L, 16));
        try { p("bad " + Long.parseLong("nope")); }
        catch (NumberFormatException e) { p("parseLong throws"); }

        p("dparse " + Double.parseDouble("1.5") + " "
          + Double.parseDouble("-2e3") + " " + Double.parseDouble("7"));
        try { p("bad " + Double.parseDouble("1.2.3")); }
        catch (NumberFormatException e) { p("parseDouble throws"); }
        try { p("bad " + Double.parseDouble("")); }
        catch (NumberFormatException e) { p("parseDouble empty throws"); }

        Double bd = new Double(2.5);
        p("dbox " + bd.doubleValue() + " " + bd.intValue() + " "
          + bd.toString() + " " + Double.valueOf("0.25").doubleValue());
        p("fparse " + Float.parseFloat("1.5") + " "
          + new Float(0.5f).floatValue() + " " + new Float(2.5f).intValue());

        p("bool " + Boolean.valueOf("true").booleanValue() + " "
          + Boolean.valueOf("TRUE").booleanValue() + " "
          + Boolean.valueOf("yes").booleanValue() + " "
          + new Boolean(true).toString());
        p("boolconst " + Boolean.TRUE.booleanValue()
          + Boolean.FALSE.booleanValue());

        p("digit " + Character.isDigit('7') + Character.isDigit('a')
          + Character.isLetter('a') + Character.isLetter('7')
          + Character.isLetterOrDigit('_'));
        p("space " + Character.isWhitespace(' ') + Character.isWhitespace('\t')
          + Character.isWhitespace('\n') + Character.isWhitespace('x'));
        p("ulcase " + Character.toUpperCase('a') + Character.toUpperCase('Z')
          + Character.toUpperCase('7') + Character.toLowerCase('Q'));
        p("chcase " + Character.isUpperCase('A') + Character.isLowerCase('A'));
        p("digitval " + Character.digit('f', 16) + " "
          + Character.digit('9', 10) + " " + Character.digit('a', 10) + " "
          + Character.forDigit(11, 16));
        Character bc = new Character('k');
        p("cbox " + bc.charValue() + " " + bc.toString() + " "
          + Character.toString('m'));

        p("mathish " + Math.abs(-3) + " " + Math.max(2, 9));
    }
}
