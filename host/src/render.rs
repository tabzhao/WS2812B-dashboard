//! 把字符串渲染到 32×8 像素（行优先 RGB）
//!
//! 用内置 5×7 ASCII 字模（font5x7.rs，源自 Adafruit GFX 公共领域字库）。
//! 「取模」就是查字模 → 逐列逐行点亮。横向滚动显示，超出右边截断。
//! 不支持中文（5×7 没有中文字模）；中文/非 ASCII 字符跳过。

use crate::font5x7::FONT5X7;

pub const COLS: usize = 32;
pub const ROWS: usize = 8;
pub const PIXELS: usize = COLS * ROWS;
pub const RGB_BYTES: usize = PIXELS * 3;

/// 每个字符占用的列数：5 列字模 + 1 列间距。
pub const CHAR_PITCH: usize = 6;

/// 把 text 渲染到 out（必须 768 字节，行优先 RGB，(0,0) 左上）。
/// 按列偏移 scroll_x 横向滚动：scroll_x=0 时第一个字符在最左；
/// scroll_x 增大则字符串向左移（露出右边新的字符）。单色 (r,g,b)。
pub fn render_text(text: &str, scroll_x: i32, r: u8, g: u8, b: u8, out: &mut [u8]) {
    assert_eq!(out.len(), RGB_BYTES);
    out.fill(0);

    for (i, ch) in text.chars().enumerate() {
        let code = ch as u32;
        if code > 255 { continue; } // 跳过非 Latin-1（中文等无 5×7 字模）
        let code = code as usize;
        let glyph = &FONT5X7[code * 5..code * 5 + 5];

        // 字符起始列 = i*CHAR_PITCH - scroll_x
        let x0 = (i as i32) * (CHAR_PITCH as i32) - scroll_x;

        for col in 0..5 {
            let x = x0 + col as i32;
            if x < 0 || x >= COLS as i32 { continue; }
            let bits = glyph[col as usize];
            // bit0 = 最上行（顶）。字模 7 行，屏 8 行，用 row 0..6，第 7 行留白。
            for row in 0..7u32 {
                if bits & (1 << row) != 0 {
                    let y = row as usize;
                    let idx = (y * COLS + x as usize) * 3;
                    out[idx] = r;
                    out[idx + 1] = g;
                    out[idx + 2] = b;
                }
            }
        }
    }
}

/// HSV → RGB。h 取 0..360，s/v 取 0..1。
/// 频谱柱子靠它做「底部绿 → 顶部红」的渐变。
pub fn hsv_to_rgb(h: f32, s: f32, v: f32) -> (u8, u8, u8) {
    let c = v * s;
    let hp = ((h % 360.0) + 360.0) % 360.0 / 60.0;
    let x = c * (1.0 - ((hp % 2.0) - 1.0).abs());
    let (r, g, b) = match hp as u32 {
        0 => (c, x, 0.0),
        1 => (x, c, 0.0),
        2 => (0.0, c, x),
        3 => (0.0, x, c),
        4 => (x, 0.0, c),
        _ => (c, 0.0, x),
    };
    let m = v - c;
    let to8 = |v: f32| (v.clamp(0.0, 1.0) * 255.0).round() as u8;
    (to8(r + m), to8(g + m), to8(b + m))
}

/// 把 32 段频谱画成柱状图写进 out（768 字节，行优先，(0,0) 左上）。
///
/// levels / peaks 都是 0..1：levels 决定柱子高度，peaks 是缓慢下落的峰值保持点
/// （画在柱顶之上那一格，白色）。柱子自底向上填充，颜色底部绿、顶部红。
pub fn render_spectrum(levels: &[f32; COLS], peaks: &[f32; COLS], out: &mut [u8]) {
    assert_eq!(out.len(), RGB_BYTES);
    out.fill(0);

    let set = |out: &mut [u8], x: usize, y: usize, r: u8, g: u8, b: u8| {
        let i = (y * COLS + x) * 3;
        out[i] = r;
        out[i + 1] = g;
        out[i + 2] = b;
    };

    for x in 0..COLS {
        // 柱高：0..ROWS 格
        let h = (levels[x].clamp(0.0, 1.0) * ROWS as f32).round() as usize;
        for k in 0..h {
            let y = ROWS - 1 - k; // 自底向上
            // y=ROWS-1（底）→ hue 120 绿；y=0（顶）→ hue 0 红
            let hue = 120.0 * (y as f32 / (ROWS - 1) as f32);
            let (r, g, b) = hsv_to_rgb(hue, 1.0, 1.0);
            set(out, x, y, r, g, b);
        }

        // 峰值白点：上升阶段 pk == h，白点就贴在柱顶那一格（覆盖柱顶颜色，
        // 保证上升时一定看得见）；柱子掉下去后 pk > h，白点悬在柱子上方；
        // 一路落到最底一行（pk = 1）为止，不再往下消失。
        let pk = (peaks[x].clamp(0.0, 1.0) * ROWS as f32).round().clamp(1.0, ROWS as f32) as usize;
        if pk >= h {
            let y = ROWS - pk;
            set(out, x, y, 255, 255, 255);
        }
    }
}

/// 滚动范围：字符串总宽 = text.len()*CHAR_PITCH；滚动一个周期 = 总宽 + COLS
/// （从屏右边进入，到完全滚出左边）。返回当前周期内的 scroll_x。
pub fn scroll_for(text: &str, elapsed_ms: u32, speed_ms_per_col: u32) -> i32 {
    let total_w = (text.chars().count() * CHAR_PITCH) as i32;
    let period = (total_w + COLS as i32) as u32;
    if period == 0 { return 0; }
    let pos = elapsed_ms / speed_ms_per_col;
    // 从 -COLS 开始（字符串在屏右边外），向左滚
    -(COLS as i32) + (pos as i32 % period as i32)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn renders_a_letter() {
        let mut buf = [0u8; RGB_BYTES];
        render_text("A", 0, 255, 255, 255, &mut buf);
        // 'A' (0x41) 在 glcdfont，第 0 列应有点（0x7E 等）
        let lit: usize = buf.chunks(3).map(|p| if p[0] > 0 { 1 } else { 0 }).sum();
        assert!(lit > 0, "letter A should light something");
    }

    #[test]
    fn empty_string_clears() {
        let mut buf = [255u8; RGB_BYTES];
        render_text("", 0, 255, 255, 255, &mut buf);
        assert!(buf.iter().all(|&v| v == 0));
    }

    /// 上升阶段（peaks == levels）白点必须画出来：贴在柱顶那一格。
    #[test]
    fn spectrum_peak_sits_on_top_while_rising() {
        let mut buf = [0u8; RGB_BYTES];
        let mut lv = [0f32; COLS];
        lv[0] = 0.5; // 4 格高
        let pk = lv; // 上升中：白点与柱顶等高
        render_spectrum(&lv, &pk, &mut buf);
        let i = ((ROWS - 4) * COLS + 0) * 3; // 第 4 格 = 柱顶
        assert_eq!(&buf[i..i + 3], &[255, 255, 255], "上升时柱顶应是白色");
    }

    /// 柱子归零后白点停在最底一行，不能消失。
    #[test]
    fn spectrum_peak_rests_on_bottom_row() {
        let mut buf = [0u8; RGB_BYTES];
        let mut lv = [0f32; COLS];
        let mut pk = [0f32; COLS];
        lv[1] = 0.0;
        pk[1] = 1.0 / ROWS as f32; // 最底一行
        render_spectrum(&lv, &pk, &mut buf);
        let i = ((ROWS - 1) * COLS + 1) * 3; // 底行
        assert_eq!(&buf[i..i + 3], &[255, 255, 255], "白点应停在最底一行");
    }
}
