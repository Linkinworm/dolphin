// SPDX-License-Identifier: GPL-2.0-or-later
#include "DolphinQt/QRCodeDialog.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>

QRCodeDialog::QRCodeDialog(const QString& url, QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("GBA Stream"));
  auto* layout = new QVBoxLayout(this);

  // Placeholder for the QR code
  m_qr_label = new QLabel(tr("QR CODE PLACEHOLDER\n\nScan this URL:"), this);
  m_qr_label->setFixedSize(200, 200);
  m_qr_label->setStyleSheet(QStringLiteral("background: white; border: 1px solid black;"));
  m_qr_label->setAlignment(Qt::AlignCenter);
  layout->addWidget(m_qr_label);

  auto* link = new QLabel(url, this);
  link->setOpenExternalLinks(true);
  link->setTextInteractionFlags(Qt::TextBrowserInteraction);
  layout->addWidget(link);

  auto* btn = new QPushButton(tr("Close"), this);
  connect(btn, &QPushButton::clicked, this, &QDialog::accept);
  layout->addWidget(btn);
}
