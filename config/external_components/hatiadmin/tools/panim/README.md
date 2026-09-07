# Pixel-Animationen (`.panim`) für das Animation-Tile

Das **Animation**-Tile zeigt eine kleine, bewusst niedrig aufgelöste ("pixelige")
Animation von der SD-Karte. Die Dateien liegen im Ordner **`/animations`** auf der
SD-Karte; im Web-Konfigurator wählst du pro Tile per Dropdown eine Datei aus.

Die Firmware lädt die winzigen Frames, skaliert sie auf dem Gerät **integer per
Nearest-Neighbour** hoch (scharfe Pixel, kein Verwaschen) und spielt sie mit einem
Timer ab. Hochskalierte Frames liegen im PSRAM, der Frame selbst ist ein einfacher
Blit – kein zusätzlicher PPA-Stress über den normalen Tile-Flush hinaus.

## Schnellstart (Demo-Animationen)

```
node tools/panim/make-demos.js
```

Erzeugt originale Demos unter `sd-card/animations/` (`plant.panim`, `ball.panim`).
Den Inhalt dieses Ordners nach **`/animations`** auf die SD-Karte kopieren, Karte
einstecken – im Tile-Typ **„Animation"** taucht die Datei im Dropdown auf.

## Eigene Animation aus einem PNG-Spritesheet

```
node tools/panim/png-to-panim.js <input.png> frames=N fps=8 scale=N [out=name.panim] [bg=#000000]
```

- **Spritesheet**: alle Frames nebeneinander in einer Reihe; Breite = `frames × Framebreite`.
- **`frames`**: Anzahl der Frames im Sheet.
- **`fps`**: Abspielgeschwindigkeit (z. B. 6–8 wirkt schön „retro").
- **`scale`**: Downsampling – ist deine Vorlage „klotzig" gezeichnet (jeder logische
  Pixel ist ein `N×N`-Block), dann wird mit `scale=N` jeder Block zu einem nativen Pixel.
- **`bg`**: diese Farbe (und voll transparente Pixel) wird als Hintergrund behandelt
  und schwarz gespeichert, damit sie mit dem schwarzen Tile-Hintergrund verschmilzt.

Voraussetzung: `npm install pngjs`.

Halte die native Framegröße klein (≤ 128 px pro Seite, ideal 16–48 px) – das Gerät
skaliert ohnehin scharf hoch.

> Achte darauf, nur Grafiken zu konvertieren, an denen du die nötigen Rechte hast.
> Es werden bewusst keine fremden/geschützten Sprites mitgeliefert.

## `.panim`-Format

Little-Endian-Header, dann rohe Frames:

| Offset | Größe | Feld |
|-------:|------:|------|
| 0  | 4 | Magic `'P','A','N','1'` |
| 4  | 2 | width (uint16) |
| 6  | 2 | height (uint16) |
| 8  | 2 | frame_count (uint16) |
| 10 | 2 | frame_ms (uint16) – Dauer pro Frame in ms |
| 12 | … | `frame_count × width·height·2` Bytes |

Jeder Pixel ist **RGB565, big-endian** abgelegt. Auf dem little-endian ESP32 ergibt
das im Speicher genau den byte-getauschten Wert, den das Display erwartet
(`LV_COLOR_FORMAT_RGB565_SWAPPED`) – dieselbe Konvention wie der JPEG-Icon-Pfad.

Encoder/Decoder-Referenz: [`panim.js`](panim.js) (Host) und
[`src/types/pixelanim/renderer.cpp`](../../src/types/pixelanim/renderer.cpp) (Firmware).
