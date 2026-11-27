/*----------------------------------------------*/
/* TJpgDec System Configurations R0.03          */
/*----------------------------------------------*/
/* P4 board: a copy of LVGL's TJpgDec (src/libs/tjpgd: ChaN's R0.03 as LVGL changed it - it writes B, G, R and has
   no working scaling, so neither is used) for the jam tracks' covers (p4_cover.c), with its settings unchanged. Its
   functions get a prefix of their own, so the two copies cannot meet at the link (LVGL's is compiled out while
   LV_USE_TJPGD is off). Only the two #if LV_USE_TJPGD and LVGL's include are changed in tjpgd.c / tjpgd.h. */
#define jd_prepare p4_jd_prepare
#define jd_decomp p4_jd_decomp
#define jd_mcu_load p4_jd_mcu_load
#define jd_mcu_output p4_jd_mcu_output
#define jd_restart p4_jd_restart

#define JD_SZBUF        512
/* Specifies size of stream input buffer */

#define JD_FORMAT       0
/* Specifies output pixel format.
/  0: RGB888 (24-bit/pix)
/  1: RGB565 (16-bit/pix)
/  2: Grayscale (8-bit/pix)
*/

#define JD_USE_SCALE    0
/* Switches output descaling feature.
/  0: Disable
/  1: Enable
*/

#define JD_TBLCLIP      1
/* Use table conversion for saturation arithmetic. A bit faster, but increases 1 KB of code size. */

#define JD_FASTDECODE   1
/* Optimization level 1: + 32-bit barrel shifter. Suitable for 32-bit MCUs. */
