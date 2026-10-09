// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>
#include <QString>

class QLabel;

class QRCodeDialog : public QDialog
{
  Q_OBJECT
public:
  explicit QRCodeDialog(const QString& url, QWidget* parent = nullptr);

private:
  QLabel* m_qr_label;
};
