// What the tool system asks of the window (rs_mainwindow.cpp). Each function checks it is on Qt's thread
// before touching a widget.
#pragma once

void RsWindowShow();
void RsWindowHide();
void RsWindowFlush();
void RsWindowShutdown();
