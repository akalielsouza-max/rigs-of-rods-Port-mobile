package org.rigsofrods.mobile;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.graphics.Color;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.ScaleGestureDetector;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.Arrays;
import java.util.List;

public class MainActivity extends Activity implements SurfaceHolder.Callback {
    private static native String nativeStart(Surface s, String logPath);
    private static native void nativeStop();
    private static native String nativeGetLog();
    private static native void nativeTouch(float dx, float dy);
    private static native void nativeZoom(float factor);
    private static native String nativeRorStart(Surface s, String baseDir, String logPath);
    private static native void nativeRorStop();
    private static native String nativeRorGetLog();

    private TextView out;
    private TextView ogreOut;
    private File statusFile;
    private Surface surface;
    private boolean ogreRunning = false;
    private boolean renderLibLoaded = false;
    private boolean rorLibLoaded = false;
    private int mode = 0; // 0 = nada, 1 = teste do OGRE, 2 = RoR
    private boolean okMarked = false;
    private float lastX;
    private float lastY;
    private boolean dragging = false;
    private ScaleGestureDetector scaleDet;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (!ogreRunning) {
                return;
            }
            String s = (mode == 2) ? nativeRorGetLog() : nativeGetLog();
            if (s.length() > 3500) {
                s = s.substring(s.length() - 3500);
            }
            ogreOut.setText(s);
            if (!okMarked && s.contains("FRAMES_OK")) {
                okMarked = true;
                write("OGRE_OK");
            }
            handler.postDelayed(this, 500);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        statusFile = new File(getFilesDir(), "load_status.txt");

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(20, 24, 30));
        root.setPadding(24, 48, 24, 24);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        Button b1 = new Button(this);
        b1.setText("Carregar RoR");
        Button b2 = new Button(this);
        b2.setText("Iniciar OGRE");
        Button b3 = new Button(this);
        b3.setText("Parar");
        Button b4 = new Button(this);
        b4.setText("Conteudo");
        LinearLayout.LayoutParams w = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        row.addView(b2, w);
        row.addView(b3, w);
        row.addView(b4, w);
        root.addView(row);
        LinearLayout row2 = new LinearLayout(this);
        row2.setOrientation(LinearLayout.HORIZONTAL);
        Button b5 = new Button(this);
        b5.setText("Iniciar RoR");
        row2.addView(b5, w);
        root.addView(row2);

        SurfaceView sv = new SurfaceView(this);
        sv.getHolder().addCallback(this);
        scaleDet = new ScaleGestureDetector(this, new ScaleGestureDetector.SimpleOnScaleGestureListener() {
            @Override
            public boolean onScale(ScaleGestureDetector d) {
                if (ogreRunning) {
                    try {
                        nativeZoom(d.getScaleFactor());
                    } catch (Throwable ignored) {
                    }
                }
                return true;
            }
        });
        sv.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent e) {
                scaleDet.onTouchEvent(e);
                if (scaleDet.isInProgress()) {
                    dragging = false;
                    return true;
                }
                int a = e.getActionMasked();
                if (a == MotionEvent.ACTION_DOWN) {
                    lastX = e.getX();
                    lastY = e.getY();
                    dragging = true;
                } else if (a == MotionEvent.ACTION_MOVE) {
                    if (dragging && ogreRunning) {
                        try {
                            nativeTouch(e.getX() - lastX, e.getY() - lastY);
                        } catch (Throwable ignored) {
                        }
                        lastX = e.getX();
                        lastY = e.getY();
                    }
                } else if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) {
                    dragging = false;
                }
                return true;
            }
        });
        root.addView(sv, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        ScrollView scroll = new ScrollView(this);
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        out = new TextView(this);
        out.setTextColor(Color.WHITE);
        out.setTextSize(12f);
        out.setTextIsSelectable(true);
        ogreOut = new TextView(this);
        ogreOut.setTextColor(Color.rgb(160, 230, 160));
        ogreOut.setTextSize(12f);
        ogreOut.setTextIsSelectable(true);
        box.addView(out);
        box.addView(ogreOut);
        scroll.addView(box);
        root.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1.3f));
        setContentView(root);

        log("Rigs of Rods Mobile 0.4 (cena 3D leve: arraste = girar, pinca = zoom)");
        log("Aparelho: " + Build.MANUFACTURER + " " + Build.MODEL
                + " | Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")"
                + " | ABIs: " + Arrays.toString(Build.SUPPORTED_ABIS));

        String prev = read();
        if ("STARTED".equals(prev)) {
            log("ATENCAO: a tentativa anterior de carregar a libRoR.so TRAVOU o app (crash nativo).");
        } else if ("OGRE_STARTED".equals(prev)) {
            log("ATENCAO: a tentativa anterior de iniciar o OGRE TRAVOU o app (crash nativo).");
        } else if ("OGRE_OK".equals(prev)) {
            log("Tentativa anterior: o OGRE desenhou quadros normalmente.");
        } else if (prev != null) {
            log("Resultado da tentativa anterior: " + prev);
        }
        showExitReasons();

        String last = readFile(new File(getFilesDir(), "ogre_log.txt"));
        if (last != null && last.length() > 0) {
            if (last.length() > 3500) {
                last = last.substring(0, 2200) + "\n[...]\n" + last.substring(last.length() - 1200);
            }
            log("--- Log salvo da ultima tentativa do OGRE ---");
            log(last);
            log("--- fim do log salvo ---");
        }
        String rl = readFile(new File(getFilesDir(), "ror_log.txt"));
        if (rl != null && rl.length() > 0) {
            if (rl.length() > 3500) {
                rl = rl.substring(0, 1500) + "\n[...]\n" + rl.substring(rl.length() - 1800);
            }
            log("--- Log salvo do iniciador do RoR ---");
            log(rl);
            log("--- fim do log do iniciador ---");
        }
        String rorLog = readFile(new File(ContentInstaller.root(this), "user/logs/RoR.log"));
        if (rorLog != null && rorLog.length() > 0) {
            if (rorLog.length() > 2000) {
                rorLog = rorLog.substring(rorLog.length() - 2000);
            }
            log("--- RoR.log (final) ---");
            log(rorLog);
            log("--- fim do RoR.log ---");
        }
        String og = readFile(new File(getFilesDir(), "ogre_log.txt.ogre.log"));
        if (og != null && og.length() > 0) {
            if (og.length() > 1800) {
                og = og.substring(og.length() - 1800);
            }
            log("--- Log interno do OGRE (final) ---");
            log(og);
            log("--- fim do log do OGRE ---");
        }

        b1.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                tryLoadRoR();
            }
        });
        b2.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startOgre();
            }
        });
        b3.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                stopOgre();
            }
        });
        b5.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startRor();
            }
        });
        b4.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                log("Instalando o conteudo do RoR (pode levar um minuto)...");
                new Thread(new Runnable() {
                    @Override
                    public void run() {
                        ContentInstaller.install(MainActivity.this, new ContentInstaller.Out() {
                            @Override
                            public void line(final String s) {
                                runOnUiThread(new Runnable() {
                                    @Override
                                    public void run() {
                                        log(s);
                                    }
                                });
                            }
                        });
                    }
                }).start();
            }
        });
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        surface = holder.getSurface();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        stopOgre();
        surface = null;
    }

    private void startOgre() {
        if (ogreRunning) {
            log("OGRE ja esta rodando.");
            return;
        }
        if (surface == null || !surface.isValid()) {
            log("A superficie de video ainda nao esta pronta.");
            return;
        }
        try {
            if (!renderLibLoaded) {
                System.loadLibrary("RoRRender");
                renderLibLoaded = true;
            }
        } catch (Throwable t) {
            log("ERRO ao carregar libRoRRender.so: " + t);
            return;
        }
        okMarked = false;
        write("OGRE_STARTED");
        try {
            String err = nativeStart(surface, new File(getFilesDir(), "ogre_log.txt").getPath());
            if (err != null && err.length() > 0) {
                log("ERRO: " + err);
                write("ERRO: " + err);
                return;
            }
        } catch (Throwable t) {
            log("ERRO: " + t);
            write("ERRO: " + t);
            return;
        }
        ogreRunning = true;
        mode = 1;
        log("Iniciando OGRE...");
        handler.post(poll);
    }

    private void startRor() {
        if (ogreRunning) {
            log("Pare o que esta rodando antes (botao Parar).");
            return;
        }
        if (surface == null || !surface.isValid()) {
            log("A superficie de video ainda nao esta pronta.");
            return;
        }
        File res = new File(ContentInstaller.root(this), "resources");
        if (!res.exists()) {
            log("Instale o conteudo primeiro (botao Conteudo).");
            return;
        }
        try {
            if (!rorLibLoaded) {
                System.loadLibrary("RoR");
                rorLibLoaded = true;
            }
        } catch (Throwable t) {
            log("ERRO ao carregar libRoR.so: " + t);
            return;
        }
        okMarked = false;
        write("ROR_STARTED");
        try {
            String err = nativeRorStart(surface, ContentInstaller.root(this).getPath(),
                    new File(getFilesDir(), "ror_log.txt").getPath());
            if (err != null && err.length() > 0) {
                log("ERRO: " + err);
                write("ERRO: " + err);
                return;
            }
        } catch (Throwable t) {
            log("ERRO: " + t);
            write("ERRO: " + t);
            return;
        }
        ogreRunning = true;
        mode = 2;
        log("Iniciando o RoR...");
        handler.post(poll);
    }

    private void stopOgre() {
        if (!ogreRunning) {
            return;
        }
        ogreRunning = false;
        handler.removeCallbacks(poll);
        try {
            if (mode == 2) {
                nativeRorStop();
                ogreOut.setText(nativeRorGetLog());
            } else {
                nativeStop();
                ogreOut.setText(nativeGetLog());
            }
        } catch (Throwable t) {
            log("ERRO ao parar: " + t);
        }
        if (!okMarked) {
            write("OGRE_PARADO_SEM_QUADROS");
        }
        log("OGRE parado.");
    }

    @Override
    protected void onDestroy() {
        stopOgre();
        super.onDestroy();
    }

    private void tryLoadRoR() {
        write("STARTED");
        log("Carregando libRoR.so...");
        new Thread(new Runnable() {
            @Override
            public void run() {
                String result;
                try {
                    System.loadLibrary("RoR");
                    result = "OK";
                } catch (Throwable t) {
                    result = "ERRO: " + t;
                }
                final String r = result;
                write(r);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        log("libRoR.so: " + r);
                    }
                });
            }
        }).start();
    }

    private void showExitReasons() {
        if (Build.VERSION.SDK_INT < 30) {
            return;
        }
        try {
            ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
            List<ApplicationExitInfo> list = am.getHistoricalProcessExitReasons(getPackageName(), 0, 3);
            for (ApplicationExitInfo i : list) {
                String name;
                if (i.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE) {
                    name = "CRASH_NATIVO";
                } else if (i.getReason() == ApplicationExitInfo.REASON_CRASH) {
                    name = "CRASH_JAVA";
                } else if (i.getReason() == ApplicationExitInfo.REASON_LOW_MEMORY) {
                    name = "POUCA_MEMORIA";
                } else {
                    name = "codigo " + i.getReason();
                }
                log("Saida anterior do app: " + name + " | sinal/status=" + i.getStatus()
                        + " | " + i.getDescription());
            }
        } catch (Throwable t) {
            log("(nao foi possivel ler o historico de saidas: " + t + ")");
        }
    }

    private void log(String s) {
        out.append(s + "\n");
    }

    private void write(String s) {
        try {
            FileOutputStream f = new FileOutputStream(statusFile);
            f.write(s.getBytes("UTF-8"));
            f.close();
        } catch (Throwable ignored) {
        }
    }

    private String readFile(File file) {
        try {
            if (!file.exists()) {
                return null;
            }
            FileInputStream f = new FileInputStream(file);
            byte[] b = new byte[(int) file.length()];
            int n = f.read(b);
            f.close();
            return new String(b, 0, Math.max(n, 0), "UTF-8");
        } catch (Throwable t) {
            return null;
        }
    }

    private String read() {
        try {
            if (!statusFile.exists()) {
                return null;
            }
            FileInputStream f = new FileInputStream(statusFile);
            byte[] b = new byte[(int) statusFile.length()];
            int n = f.read(b);
            f.close();
            return new String(b, 0, Math.max(n, 0), "UTF-8");
        } catch (Throwable t) {
            return null;
        }
    }
}
// FIM_DO_ARQUIVO
            
