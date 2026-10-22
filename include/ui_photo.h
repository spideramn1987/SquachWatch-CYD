// SquachWatch-CYD — photo frame mode
//
// Slideshow from /photos on the SD card (.jpg/.jpeg/.bmp), with optional
// corner clock + battery gauge and Desk Mode's compact detection alert card.
// Untouched photo pixels stream out to the panel in true 16-bit RGB565 via
// uiPhotoWire565() while UI overlays composite seamlessly on top.
#pragma once
#include <TFT_eSPI.h>
#include <stdint.h>
#include "detection.h"

void uiPhotoInit(TFT_eSPI& t, uint32_t now);
void uiPhotoRescan();
uint16_t uiPhotoCount();

void uiPhotoTick(TFT_eSPI& t, uint32_t now, const DetectionEngine& eng);
void uiPhotoStep(int dir, uint32_t now);

bool uiPhotoHudUp(uint32_t now);
void uiPhotoToggleHud(uint32_t now);

bool uiPhotoHitBack(int x, int y, int screenW, int screenH, uint32_t now);
bool uiPhotoHitSettings(int x, int y, int screenW, int screenH, uint32_t now);
bool uiPhotoHitNext(int x, int y, int screenW, int screenH, uint32_t now);
int  uiPhotoHitEdge(int x, int y, int screenW, int screenH);

uint8_t uiPhotoStops(int screenW, int screenH, uint32_t now, int16_t* xs, int16_t* ys, uint8_t cap);

// True 16-bit RGB565 photo buffer hook for FramePush::pushRowsDma().
void            uiPhotoSetWireActive(bool on);
const uint16_t* uiPhotoWire565(const uint8_t*& match8, int32_t w, int32_t h);

