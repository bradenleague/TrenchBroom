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

#include "ui/AgentInspector.h"

#include "ui/MapDocument.h"

#include "mdl/Map.h"

#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace tb::ui
{

// ---------------------------------------------------------------------------
// Path to agent_api.py — resolved relative to quake-map-db project
// ---------------------------------------------------------------------------

namespace
{
const auto AgentScriptPath =
  QString{"%1/dev/quake-map-db/scripts/agent_api.py"}.arg(QDir::homePath());
const auto AgentPythonPath =
  QString{"%1/dev/quake-map-db/.venv/bin/python3"}.arg(QDir::homePath());
} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AgentInspector::AgentInspector(MapDocument& document, QWidget* parent)
  : TabBookPage{parent}
  , m_document{document}
{
  createGui();
}

AgentInspector::~AgentInspector()
{
  stopAgent();
}

// ---------------------------------------------------------------------------
// GUI setup
// ---------------------------------------------------------------------------

void AgentInspector::createGui()
{
  // Chat log — read-only, scrollable HTML
  m_chatLog = new QTextBrowser{};
  m_chatLog->setReadOnly(true);
  m_chatLog->setOpenExternalLinks(false);
  m_chatLog->setPlaceholderText("Agent chat will appear here...");
  m_chatLog->document()->setDefaultStyleSheet(
    "body { font-family: monospace; font-size: 10pt; }"
    ".user { color: #2196F3; }"
    ".agent { color: #E0E0E0; }"
    ".tool-call { color: #9E9E9E; font-family: monospace; font-size: 9pt; }"
    ".tool-ok { color: #4CAF50; font-size: 9pt; }"
    ".tool-err { color: #F44336; font-size: 9pt; }"
    ".error { color: #F44336; font-weight: bold; }");

  // Status label
  m_statusLabel = new QLabel{"Ready"};
  m_statusLabel->setStyleSheet("color: #9E9E9E; padding: 2px 4px;");

  // Input field + send button
  m_inputField = new QLineEdit{};
  m_inputField->setPlaceholderText("Describe what you want to build...");
  connect(
    m_inputField, &QLineEdit::returnPressed, this, [this]() {
      sendMessage(m_inputField->text());
    });

  m_sendButton = new QPushButton{"Send"};
  connect(m_sendButton, &QPushButton::clicked, this, [this]() {
    sendMessage(m_inputField->text());
  });

  auto* inputLayout = new QHBoxLayout{};
  inputLayout->setContentsMargins(0, 0, 0, 0);
  inputLayout->addWidget(m_inputField, 1);
  inputLayout->addWidget(m_sendButton);

  auto* layout = new QVBoxLayout{};
  layout->setContentsMargins(4, 4, 4, 4);
  layout->setSpacing(4);
  layout->addWidget(m_chatLog, 1);
  layout->addWidget(m_statusLabel);
  layout->addLayout(inputLayout);
  setLayout(layout);
}

// ---------------------------------------------------------------------------
// Agent process management
// ---------------------------------------------------------------------------

void AgentInspector::startAgent()
{
  if (m_agentProcess)
  {
    return;
  }

  m_agentProcess = new QProcess{this};

  // Set environment: unbuffered output + LD_LIBRARY_PATH for ericw-tools
  auto env = QProcessEnvironment::systemEnvironment();
  env.insert("PYTHONUNBUFFERED", "1");
  if (!env.contains("LD_LIBRARY_PATH"))
  {
    env.insert(
      "LD_LIBRARY_PATH",
      QString{"%1/.local/lib"}.arg(QDir::homePath()));
  }
  m_agentProcess->setProcessEnvironment(env);

  // Working directory = quake-map-db project root
  m_agentProcess->setWorkingDirectory(
    QString{"%1/dev/quake-map-db"}.arg(QDir::homePath()));

  connect(
    m_agentProcess,
    &QProcess::readyReadStandardOutput,
    this,
    &AgentInspector::handleAgentOutput);
  connect(
    m_agentProcess,
    &QProcess::readyReadStandardError,
    this,
    &AgentInspector::handleAgentError);
  connect(
    m_agentProcess,
    qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
    this,
    [this](int exitCode, QProcess::ExitStatus) {
      handleProcessFinished(exitCode);
    });

  // Use venv python if it exists, otherwise fall back to system python3
  auto python = QFile::exists(AgentPythonPath) ? AgentPythonPath
                                               : QString{"python3"};
  m_agentProcess->start(python, QStringList{AgentScriptPath});
  setStatus("Agent starting...");
}

void AgentInspector::stopAgent()
{
  if (!m_agentProcess)
  {
    return;
  }

  m_agentProcess->closeWriteChannel();

  if (!m_agentProcess->waitForFinished(3000))
  {
    m_agentProcess->kill();
    m_agentProcess->waitForFinished(1000);
  }

  delete m_agentProcess;
  m_agentProcess = nullptr;
  m_stdoutBuffer.clear();
}

void AgentInspector::ensureAgentRunning()
{
  if (!m_agentProcess
      || m_agentProcess->state() == QProcess::NotRunning)
  {
    if (m_agentProcess)
    {
      delete m_agentProcess;
      m_agentProcess = nullptr;
      m_stdoutBuffer.clear();
    }
    startAgent();
  }
}

// ---------------------------------------------------------------------------
// Message sending
// ---------------------------------------------------------------------------

void AgentInspector::sendMessage(const QString& message)
{
  auto trimmed = message.trimmed();
  if (trimmed.isEmpty())
  {
    return;
  }

  // Guard: map must be saved to disk
  if (!m_document.map().persistent())
  {
    appendError("Save the map first — the agent needs a file path to work with.");
    return;
  }

  ensureAgentRunning();

  appendUserMessage(trimmed);
  m_inputField->clear();
  m_inputField->setEnabled(false);
  m_sendButton->setEnabled(false);

  // Build JSON line request
  auto request = QJsonObject{};
  request["type"] = "chat";
  request["message"] = trimmed;
  request["map_path"] = currentMapPath();

  auto jsonLine = QJsonDocument{request}.toJson(QJsonDocument::Compact) + "\n";
  m_agentProcess->write(jsonLine);
}

// ---------------------------------------------------------------------------
// Process output handling
// ---------------------------------------------------------------------------

void AgentInspector::handleAgentOutput()
{
  m_stdoutBuffer.append(m_agentProcess->readAllStandardOutput());

  // Process complete lines
  while (true)
  {
    auto newlineIdx = m_stdoutBuffer.indexOf('\n');
    if (newlineIdx < 0)
    {
      break;
    }

    auto line = m_stdoutBuffer.left(newlineIdx).trimmed();
    m_stdoutBuffer.remove(0, newlineIdx + 1);

    if (!line.isEmpty())
    {
      processJsonLine(line);
    }
  }
}

void AgentInspector::handleAgentError()
{
  auto stderr_data = m_agentProcess->readAllStandardError();
  if (!stderr_data.isEmpty())
  {
    // Log stderr but don't show raw output to user — it's usually
    // Python tracebacks or SDK debug output
    qWarning("Agent stderr: %s", stderr_data.constData());
  }
}

void AgentInspector::handleProcessFinished(int exitCode)
{
  if (exitCode != 0)
  {
    setStatus("Agent stopped — send a message to restart");
  }
  else
  {
    setStatus("Agent stopped");
  }

  m_inputField->setEnabled(true);
  m_sendButton->setEnabled(true);
}

void AgentInspector::processJsonLine(const QByteArray& line)
{
  auto doc = QJsonDocument::fromJson(line);
  if (doc.isNull())
  {
    qWarning("Agent: invalid JSON line: %s", line.constData());
    return;
  }

  auto obj = doc.object();
  auto type = obj["type"].toString();

  if (type == "status")
  {
    setStatus(obj["status"].toString());
  }
  else if (type == "tool_call")
  {
    auto input = QJsonDocument{obj["input"].toObject()}.toJson(QJsonDocument::Compact);
    appendToolCall(obj["name"].toString(), QString::fromUtf8(input));
  }
  else if (type == "tool_result")
  {
    auto success = obj["success"].toBool();
    auto output = success ? obj["output"].toString() : obj["error"].toString();
    appendToolResult(obj["name"].toString(), success, output);
  }
  else if (type == "text")
  {
    appendAgentText(obj["text"].toString());
  }
  else if (type == "done")
  {
    setStatus("Ready");
    m_inputField->setEnabled(true);
    m_sendButton->setEnabled(true);
    m_inputField->setFocus();
  }
  else if (type == "error")
  {
    appendError(obj["message"].toString());
    setStatus("Ready");
    m_inputField->setEnabled(true);
    m_sendButton->setEnabled(true);
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

QString AgentInspector::currentMapPath() const
{
  return QString::fromStdString(m_document.map().path().string());
}

// ---------------------------------------------------------------------------
// Chat log formatting
// ---------------------------------------------------------------------------

void AgentInspector::appendUserMessage(const QString& text)
{
  m_chatLog->append(
    QString{"<p class='user'><b>You:</b> %1</p>"}.arg(text.toHtmlEscaped()));
}

void AgentInspector::appendAgentText(const QString& text)
{
  // Convert newlines to <br> for multi-line responses
  auto html = text.toHtmlEscaped().replace("\n", "<br>");
  m_chatLog->append(
    QString{"<p class='agent'><b>Agent:</b> %1</p>"}.arg(html));
}

void AgentInspector::appendToolCall(const QString& name, const QString& input)
{
  m_chatLog->append(
    QString{"<p class='tool-call'>&#128295; %1(%2)</p>"}
      .arg(name.toHtmlEscaped(), input.toHtmlEscaped()));
}

void AgentInspector::appendToolResult(
  const QString& name, bool success, const QString& output)
{
  if (success)
  {
    auto truncated = output.length() > 200
                       ? output.left(200) + "..."
                       : output;
    m_chatLog->append(
      QString{"<p class='tool-ok'>&#9989; %1: %2</p>"}
        .arg(name.toHtmlEscaped(), truncated.toHtmlEscaped()));
  }
  else
  {
    m_chatLog->append(
      QString{"<p class='tool-err'>&#10060; %1: %2</p>"}
        .arg(name.toHtmlEscaped(), output.toHtmlEscaped()));
  }
}

void AgentInspector::appendError(const QString& message)
{
  m_chatLog->append(
    QString{"<p class='error'>Error: %1</p>"}.arg(message.toHtmlEscaped()));
}

void AgentInspector::setStatus(const QString& status)
{
  m_statusLabel->setText(status);
}

} // namespace tb::ui
