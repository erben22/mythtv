#ifndef LOGGINGSERVER_H_
#define LOGGINGSERVER_H_

#include <QMutexLocker>
#include <QSocketNotifier>
#include <QMutex>
#include <QQueue>
#include <QTime>

#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "mythbaseexp.h"  //  MBASE_PUBLIC , etc.
#include "verbosedefs.h"
#include "mthread.h"
#include "nzmqt.hpp"

#define LOGLINE_MAX (2048-120)

class QString;
class MSqlQuery;
class LoggingItem;

MBASE_PUBLIC void logServerStart(void);
MBASE_PUBLIC void logServerStop(void);
void logServerWait(void);

/// \brief Base class for the various logging mechanisms
class LoggerBase : public QObject {
    Q_OBJECT

  public:
    /// \brief LoggerBase Constructor
    LoggerBase(const char *string);
    /// \brief LoggerBase Deconstructor
    virtual ~LoggerBase();
    /// \brief Process a log message for the logger instance
    /// \param item LoggingItem containing the log message to process
    virtual bool logmsg(LoggingItem *item) = 0;
    /// \brief Reopen the log file to facilitate log rolling
    virtual void reopen(void) = 0;
    /// \brief Stop logging to the database
    virtual void stopDatabaseAccess(void) { }
    virtual void setupZMQSocket(void) = 0;
  protected:
    char *m_handle; ///< semi-opaque handle for identifying instance
};

/// \brief File-based logger - used for logfiles and console
class FileLogger : public LoggerBase {
    Q_OBJECT

  public:
    FileLogger(const char *filename);
    ~FileLogger();
    bool logmsg(LoggingItem *item);
    void reopen(void);
    void setupZMQSocket(void);
  private:
    bool m_opened;      ///< true when the logfile is opened
    int  m_fd;          ///< contains the file descriptor for the logfile
    nzmqt::ZMQSocket *m_zmqSock;  ///< ZeroMQ feeding socket
  protected slots:
    void receivedMessage(const QList<QByteArray>&);
};

#ifndef _WIN32
/// \brief Syslog-based logger (not available in Windows)
class SyslogLogger : public LoggerBase {
    Q_OBJECT

  public:
    SyslogLogger();
    ~SyslogLogger();
    bool logmsg(LoggingItem *item);
    /// \brief Unused for this logger.
    void reopen(void) { };
    void setupZMQSocket(void);
  private:
    char *m_application;    ///< C-string of the application name
    bool m_opened;          ///< true when syslog channel open.
    nzmqt::ZMQSocket *m_zmqSock;  ///< ZeroMQ feeding socket
  protected slots:
    void receivedMessage(const QList<QByteArray>&);
};
#endif

class DBLoggerThread;

/// \brief Database logger - logs to the MythTV database
class DatabaseLogger : public LoggerBase {
    Q_OBJECT

    friend class DBLoggerThread;
  public:
    DatabaseLogger(const char *table);
    ~DatabaseLogger();
    bool logmsg(LoggingItem *item);
    void reopen(void) { };
    virtual void stopDatabaseAccess(void);
    void setupZMQSocket(void);
  protected:
    bool logqmsg(MSqlQuery &query, LoggingItem *item);
    void prepare(MSqlQuery &query);
  private:
    bool isDatabaseReady(void);
    bool tableExists(const QString &table);

    DBLoggerThread *m_thread;   ///< The database queue handling thread
    QString m_query;            ///< The database query to insert log messages
    bool m_opened;              ///< The database is opened
    bool m_loggingTableExists;  ///< The desired logging table exists
    bool m_disabled;            ///< DB logging is temporarily disabled
    QTime m_disabledTime;       ///< Time when the DB logging was disabled
    QTime m_errorLoggingTime;   ///< Time when DB error logging was last done
    static const int kMinDisabledTime; ///< Minimum time to disable DB logging
                                       ///  (in ms)
    nzmqt::ZMQSocket *m_zmqSock;  ///< ZeroMQ feeding socket
  protected slots:
    void receivedMessage(const QList<QByteArray>&);
};

typedef QList<QByteArray> LogMessage;
typedef QList<LogMessage *> LogMessageList;

/// \brief The logging thread that received the messages from the clients via
///        ZeroMQ and dispatches each LoggingItem to each logger instance via
///        ZeroMQ.
class LogServerThread : public QObject, public MThread
{
    Q_OBJECT;

    friend void logSighup(int signum);
  public:
    LogServerThread();
    ~LogServerThread();
    void run(void);
    void stop(void);
    nzmqt::ZMQContext *getZMQContext(void) { return m_zmqContext; };
  private:
    bool m_aborted;                  ///< Flag to abort the thread.
    nzmqt::ZMQContext *m_zmqContext; ///< ZeroMQ context
    nzmqt::ZMQSocket *m_zmqInSock;   ///< ZeroMQ feeding socket
    nzmqt::ZMQSocket *m_zmqPubSock;  ///< ZeroMQ publishing socket

    QTimer *m_heartbeatTimer;        ///< 1s repeating timer for client
                                     ///  heartbeats
    QTimer *m_shutdownTimer;         ///< 5 min timer to shut down if no clients
    QSocketNotifier *m_sighupNotifier;  ///< Notifier to synchronize to UNIX
                                        ///  signal SIGHUP safely

    void forwardMessage(LogMessage *msg);
    void pingClient(QString clientId);
  protected slots:
    void receivedMessage(const QList<QByteArray>&);
    void checkHeartBeats(void);
    void shutdownTimerExpired(void);
    void handleSigHup(void);
};

class QWaitCondition;
#define MAX_QUEUE_LEN 1000

/// \brief Thread that manages the queueing of logging inserts for the database.
///        The database logging gets throttled if it gets overwhelmed, and also
///        during startup.  Having a second queue allows the rest of the
///        logging to remain in sync and to allow for burstiness in the
///        database due to things like scheduler runs.
class DBLoggerThread : public MThread
{
  public:
    DBLoggerThread(DatabaseLogger *logger);
    ~DBLoggerThread();
    void run(void);
    void stop(void);
    /// \brief Enqueues a LoggingItem onto the queue for the thread to 
    ///        consume.
    bool enqueue(LoggingItem *item) 
    { 
        QMutexLocker qLock(&m_queueMutex); 
        if (!m_aborted)
            m_queue->enqueue(item); 
        return true; 
    }

    /// \brief Indicates when the queue is full
    /// \return true when the queue is full
    bool queueFull(void)
    {
        QMutexLocker qLock(&m_queueMutex); 
        return (m_queue->size() >= MAX_QUEUE_LEN);
    }
  private:
    DatabaseLogger *m_logger;       ///< The associated logger instance
    QMutex m_queueMutex;            ///< Mutex for protecting the queue
    QQueue<LoggingItem *> *m_queue; ///< Queue of LoggingItems to insert
    QWaitCondition *m_wait;         ///< Wait condition used for waiting
                                    ///  for the queue to not be full.
                                    ///  Protected by m_queueMutex
    volatile bool m_aborted;        ///< Used during shutdown to indicate
                                    ///  that the thread should stop ASAP.
                                    ///  Protected by m_queueMutex
};

extern LogServerThread *logServerThread;

#endif

/*
 * vim:ts=4:sw=4:ai:et:si:sts=4
 */
