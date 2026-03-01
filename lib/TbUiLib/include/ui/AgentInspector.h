/*
 Copyright (C) 2010 Kristian Duske

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "ui/TabBook.h"

#include <QByteArray>
#include <QProcess>

class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;

namespace tb::ui
{
class MapDocument;

class AgentInspector : public TabBookPage
{
  Q_OBJECT
private:
  MapDocument& m_document;

  QTextBrowser* m_chatLog = nullptr;
  QLineEdit* m_inputField = nullptr;
  QPushButton* m_sendButton = nullptr;
  QLabel* m_statusLabel = nullptr;

  QProcess* m_agentProcess = nullptr;
  QByteArray m_stdoutBuffer;

public:
  explicit AgentInspector(MapDocument& document, QWidget* parent = nullptr);
  ~AgentInspector() override;

private:
  void createGui();
  void startAgent();
  void stopAgent();
  void ensureAgentRunning();

  void sendMessage(const QString& message);
  void handleAgentOutput();
  void handleAgentError();
  void handleProcessFinished(int exitCode);
  void processJsonLine(const QByteArray& line);

  QString currentMapPath() const;

  void appendUserMessage(const QString& text);
  void appendAgentText(const QString& text);
  void appendToolCall(const QString& name, const QString& input);
  void appendToolResult(const QString& name, bool success, const QString& output);
  void appendError(const QString& message);
  void setStatus(const QString& status);
};

} // namespace tb::ui
