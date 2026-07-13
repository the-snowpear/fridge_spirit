// 冰箱小精灵 WKS70WSV114-WCT MIPI-DSI 独立点屏测试接口。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 初始化 EK79007 面板，并长期循环显示单色全屏和彩条图案；函数不返回。
void fridge_display_mipi_test_run(void);

#ifdef __cplusplus
}
#endif
