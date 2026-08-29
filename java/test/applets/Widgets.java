/* An applet in the shape the ones from the wild take: build a control panel in
 * init(), then draw.
 *
 * The point of this test is not that the widgets appear - they do not, and
 * ps_jawt.c says why. It is that every one of these calls returns, so that
 * paint() runs at all, and that everything the applet sets it can read back.
 * Thirteen applets in the compatibility corpus died somewhere in an
 * init() that looked like this one, before paint() was ever reached.
 *
 * The colours drawn are read out of the widgets rather than written as
 * literals, so a shell that quietly forgot what it was told shows up as the
 * wrong picture rather than as a passing test.
 */
import java.applet.Applet;
import java.awt.*;

public class Widgets extends Applet {
    Button    go;
    TextField field;
    Choice    pick;
    Checkbox  flag;
    Label     tag;
    Panel     bar;
    WCanvas   surface;
    int       failures;

    boolean freshList() {
        java.awt.List l = new java.awt.List();

        l.addItem("a");
        l.addItem("b");
        if(l.getSelectedIndex() != -1)
            return false;
        l.select(1);
        return l.getSelectedItem().equals("b") && l.getItemCount() == 2;
    }

    void check(String what, boolean ok) {
        if(!ok)
            failures++;
        System.out.println((ok ? "ok   " : "FAIL ") + what);
    }

    public void init() {
        setLayout(new BorderLayout());

        bar = new Panel();
        bar.setLayout(new FlowLayout(FlowLayout.LEFT, 3, 4));

        go    = new Button("Go");
        field = new TextField("11", 6);
        pick  = new Choice();
        flag  = new Checkbox("loop", true);
        tag   = new Label("count", Label.CENTER);

        pick.addItem("red");
        pick.addItem("green");

        bar.add(go);
        bar.add(field);
        bar.add(pick);
        bar.add(flag);
        bar.add(tag);

        add("North", bar);
        add(new Scrollbar(Scrollbar.HORIZONTAL, 0, 10, 0, 100),
            BorderLayout.SOUTH);

        /* A Panel subclass that builds its own widgets. This is the shape half
         * the wild applets take, and it is the one that loads late: WBar is
         * not read off disc until this line runs, so its constant pool is not
         * available when the shell classes are registered. */
        WBar sub = new WBar();

        check("late-loaded Panel subclass built its widgets",
              sub.getComponentCount() == 2 && sub.first().equals("A"));
        add(sub, BorderLayout.EAST);

        surface = new WCanvas();
        surface.setBackground(Color.blue);
        add(surface);

        /* Everything below is a value this applet put in and takes back out.
         * The drawing in paint() depends on three of them. */
        check("Button.getLabel", go.getLabel().equals("Go"));
        go.setLabel("Stop");
        check("Button.setLabel", go.getLabel().equals("Stop"));

        check("TextField.getText", field.getText().equals("11"));
        field.setText("37");
        check("TextField.setText", field.getText().equals("37"));
        check("TextField.getColumns", field.getColumns() == 6);
        field.setEditable(false);
        check("TextField.isEditable", !field.isEditable());

        check("Choice.getItemCount", pick.getItemCount() == 2);
        check("Choice selects the first item added",
              pick.getSelectedIndex() == 0);
        pick.select(1);
        check("Choice.select", pick.getSelectedItem().equals("green"));

        check("Checkbox.getState", flag.getState());
        flag.setState(false);
        check("Checkbox.setState", !flag.getState());

        check("Label.getText", tag.getText().equals("count"));
        check("Label.getAlignment", tag.getAlignment() == Label.CENTER);

        check("Panel.getComponentCount", bar.getComponentCount() == 5);
        bar.remove(tag);
        check("Container.remove", bar.getComponentCount() == 4);

        check("Canvas.getBackground round-trips",
              surface.getBackground().getRGB() == Color.blue.getRGB());

        FlowLayout fl = new FlowLayout(FlowLayout.RIGHT, 7, 8);
        check("FlowLayout.getAlignment", fl.getAlignment() == FlowLayout.RIGHT);
        check("FlowLayout.getHgap", fl.getHgap() == 7);
        check("FlowLayout.getVgap", fl.getVgap() == 8);

        GridLayout gl = new GridLayout(2, 3, 1, 2);
        check("GridLayout.getRows", gl.getRows() == 2);
        check("GridLayout.getColumns", gl.getColumns() == 3);

        Scrollbar sb = new Scrollbar();
        check("Scrollbar defaults to 0..100 vertical",
              sb.getMinimum() == 0 && sb.getMaximum() == 100 &&
              sb.getOrientation() == Scrollbar.VERTICAL);
        sb.setValue(200);
        check("Scrollbar clamps to maximum - visibleAmount",
              sb.getValue() == 90);

        check("BorderLayout.CENTER", BorderLayout.CENTER.equals("Center"));

        GridBagLayout      gb = new GridBagLayout();
        GridBagConstraints gc = new GridBagConstraints();
        check("GridBagConstraints defaults", gc.gridx == -1 &&
              gc.gridwidth == 1 && gc.anchor == GridBagConstraints.CENTER &&
              gc.fill == GridBagConstraints.NONE);
        gc.gridx = 2;
        gc.fill  = GridBagConstraints.BOTH;
        gc.insets = new Insets(1, 2, 3, 4);
        check("GridBagConstraints fields round-trip",
              gc.gridx == 2 && gc.fill == 1 && gc.insets.left == 2 &&
              gc.insets.right == 4);
        gb.setConstraints(go, gc);

        Insets in = getInsets();
        check("getInsets", in.top == 0 && in.bottom == 0);

        Frame f = new Frame("popup");
        check("Frame.getTitle", f.getTitle().equals("popup"));
        check("Component.getParent is null here", go.getParent() == null);
        check("getToolkit", getToolkit() != null);

        setCursor(new Cursor(Cursor.HAND_CURSOR));
        check("Cursor.getType", getCursor().getType() == Cursor.HAND_CURSOR);

        CheckboxGroup grp = new CheckboxGroup();
        Checkbox      one = new Checkbox("one", grp, true);
        check("grouped Checkbox keeps its initial state", one.getState());

        /* The overloads whose argument is an int where another overload has a
         * String. A shell that reads the descriptor wrongly dereferences the
         * int as an object here rather than failing a comparison. */
        check("TextField(int)", new TextField(10).getColumns() == 10);
        check("TextField(String) counts its own columns",
              new TextField("abcd").getColumns() == 4);
        check("Button()", new Button().getLabel().equals(""));
        check("Label()", new Label().getText().equals(""));
        check("TextArea(String,int,int)",
              new TextArea("t", 4, 5).getRows() == 4);
        check("empty Choice has nothing selected",
              new Choice().getSelectedIndex() == -1);
        check("empty List has nothing selected",
              new java.awt.List().getSelectedItem() == null);
        check("List does not select on add", freshList());

        /* Enough throwaway widgets to run the collector several times over,
         * because each one carries a C-side state block the sweep has to free
         * and the widgets still in use must survive it.
         *
         * Every result below goes into a local before it is used. That is not
         * style: a collection during a native call frees anything living only
         * on the operand stack, which is an interpreter bug this loop is quite
         * capable of provoking. See applets/GcStack.java. */
        for(int i = 0; i < 3000; i++) {
            Button junk = new Button();

            junk.setLabel(tag.getText());
        }

        String  lbl  = go.getLabel();
        String  txt  = field.getText();
        String  item = pick.getSelectedItem();

        check("widgets outlive a collection", lbl.equals("Stop") &&
              txt.equals("37") && item.equals("green"));

        System.out.println(failures == 0 ? "widgets: all ok"
                                         : "widgets: FAILURES");
    }

    public void paint(Graphics g) {
        /* Reached only because none of the above threw. The number and the
         * colour both come back out of widgets. */
        int n = 0;
        String s = field.getText();

        for(int i = 0; i < s.length(); i++)
            n = n * 10 + (s.charAt(i) - '0');

        g.setColor(pick.getSelectedItem().equals("green") ? Color.green
                                                          : Color.red);
        g.fillRect(20, 20, n * 2, 40);

        g.setColor(surface.getBackground());
        g.fillRect(20, 70, n * 2, 20);

        g.setColor(failures == 0 ? Color.white : Color.magenta);
        g.drawString(go.getLabel() + " " + n, 20, 120);
    }
}

/* A control panel as its own class, which is how most of the wild applets do
 * it - and the case where the widget classes are asked for by a class that was
 * not loaded when the applet's constructor ran. */
class WBar extends Panel {
    Button a, b;

    WBar() {
        setLayout(new GridLayout(1, 2, 2, 2));
        a = new Button("A");
        b = new Button("B");
        add(a);
        add(b);
    }

    String first() {
        return a.getLabel();
    }
}

/* Two of the sixty-three subclass Canvas to get something to draw on, so the
 * path has to work even though nothing ever calls this paint(). */
class WCanvas extends Canvas {
    WCanvas() {
        super();
    }

    public void paint(Graphics g) {
        g.fillRect(0, 0, 4, 4);
    }
}
