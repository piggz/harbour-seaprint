#ifndef CONVERTCHECKER_H
#define CONVERTCHECKER_H
#include <QObject>
#include <QMutex>

class ConvertChecker : public QObject
{
    Q_OBJECT
public:
    static ConvertChecker* instance();
    Q_PROPERTY(bool pdf MEMBER _pdf)

signals:
protected:
private:
    static ConvertChecker* m_Instance;

    ConvertChecker();
    ~ConvertChecker();
    ConvertChecker(const ConvertChecker &);
    ConvertChecker& operator=(const ConvertChecker &);

    bool _pdf;
};

#endif // CONVERTCHECKER_H
