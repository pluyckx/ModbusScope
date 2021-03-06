#include <QApplication>
#include <QThread>
#include <QCoreApplication>
#include <QDebug>
#include "modbusmaster.h"
#include "settingsmodel.h"
#include "guimodel.h"
#include <modbus.h>

#include "errno.h"

ModbusMaster::ModbusMaster(SettingsModel * pSettingsModel, GuiModel * pGuiModel, QObject *parent) :
    QObject(parent),
    _pThread(NULL)
{
    // NEVER create object with new here

    qRegisterMetaType<ModbusResult>("ModbusResult");
    qRegisterMetaType<QMap<quint16, ModbusResult> >("QMap<quint16, ModbusResult>");

    _pSettingsModel = pSettingsModel;
    _pGuiModel = pGuiModel;
}

ModbusMaster::~ModbusMaster()
{
    _pThread = NULL;
    _pSettingsModel = NULL;
    _pGuiModel = NULL;
}

void ModbusMaster::startThread()
{
    if(_pThread == NULL)
    {
        _pThread = new QThread();
        _pThread->start();
        connect(_pThread, SIGNAL(finished()), _pThread, SLOT(deleteLater()));
        connect(_pThread, SIGNAL(finished()), this, SLOT(stopped()));

        moveToThread(_pThread);
    }
}

void ModbusMaster::wait()
{
    if(_pThread)
    {
        _pThread->wait();
    }
}

void ModbusMaster::stopThread()
{
    if(_pThread)
    {
        _pThread->quit();
    }
}

void ModbusMaster::stopped()
{
    /* thread is deleted using a connection between thread->finished and thread->deleteLater */
    _pThread = NULL;

    emit threadStopped();
}

void ModbusMaster::readRegisterList(QList<quint16> registerList)
{
    QMap<quint16, ModbusResult> resultMap;

    quint32 success = 0;
    quint32 error = 0;

    /* Open port */
    modbus_t * pCtx = openPort(_pSettingsModel->ipAddress(), _pSettingsModel->port());
    if (pCtx)
    {
        /* Set modbus slave */
        modbus_set_slave(pCtx, _pSettingsModel->slaveId());

        // Disable byte time-out
        uint32_t sec = -1;
        uint32_t usec = 0;
        modbus_set_byte_timeout(pCtx, sec, usec);

        // Set response timeout
        sec = _pSettingsModel->timeout() / 1000;
        usec = (_pSettingsModel->timeout() % 1000) * 1000uL;
        modbus_set_response_timeout(pCtx, sec, usec);

        // Do optimized reads
        qint32 regIndex = 0;
        while (regIndex < registerList.size())
        {
            quint32 count = 0;

            // get number of subsequent registers
            if (
                    ((registerList.size() - regIndex) > 1)
                    && (_pSettingsModel->consecutiveMax() > 1)
                )
            {
                bool bSubsequent;
                do
                {
                    bSubsequent = false;

                    // if next is current + 1, dan subsequent = true
                    if (registerList.at(regIndex + count + 1) == registerList.at(regIndex + count) + 1)
                    {
                        bSubsequent = true;
                        count++;
                    }

                    // Break loop when end of list
                    if ((regIndex + count) >= ((uint)registerList.size() - 1))
                    {
                        break;
                    }

                    // Limit number of register in 1 read
                    if (count > (_pSettingsModel->consecutiveMax() - 1u - 1u))
                    {
                        break;
                    }

                } while(bSubsequent == true);
            }

            // At least one register
            count++;

            // Read registers
            QList<quint16> registerDataList;
            qint32 returnCode = readRegisters(pCtx, registerList.at(regIndex) - 40001, count, &registerDataList);
            if (returnCode == 0)
            {
                success++;
                for (uint i = 0; i < count; i++)
                {
                    const quint16 registerAddr = registerList.at(regIndex) + i;
                    const ModbusResult result = ModbusResult(registerDataList[i], true);
                    resultMap.insert(registerAddr, result);
                }
            }
            else
            {                
                /* only split on specific modbus exception (invalid value and invalid address) */
                if (
                        (returnCode == (MODBUS_ENOBASE + MODBUS_EXCEPTION_ILLEGAL_DATA_ADDRESS))
                        || (returnCode == (MODBUS_ENOBASE + MODBUS_EXCEPTION_ILLEGAL_DATA_VALUE))
                    )
                {

                    /* Consecutive read failed */
                    if (count == 1)
                    {
                        /* Log error */
                        error++;
                        const quint16 registerAddr = registerList.at(regIndex);
                        const ModbusResult result = ModbusResult(0, false);
                        resultMap.insert(registerAddr, result);
                    }
                    else
                    {
                        error++;

                        /* More than one => read all separately */
                        for (quint32 i = 0; i < count; i++)
                        {
                            const quint16 registerAddr = registerList.at(regIndex + i);
                            if (readRegisters(pCtx, registerAddr - 40001, 1, &registerDataList) == 0)
                            {
                                success++;
                                const ModbusResult result = ModbusResult(registerDataList[0], true);
                                resultMap.insert(registerAddr, result);
                            }
                            else
                            {
                                /* Log error */
                                error++;
                                const ModbusResult result = ModbusResult(0, false);
                                resultMap.insert(registerAddr, result);
                            }
                        }
                    }
                }
                else
                {
                    error++;

                    for (qint32 i = 0; i < registerList.size(); i++)
                    {

                        const quint16 registerAddr = registerList.at(i);
                        const ModbusResult result = ModbusResult(0, false);
                        resultMap.insert(registerAddr,result);
                    }

                    break;
                }
            }

            // Set register index to next register
            regIndex += count;
        }

        closePort(pCtx); /* Close port */
    }
    else
    {
        error++;

        for (qint32 i = 0; i < registerList.size(); i++)
        {
            const quint16 registerAddr = registerList.at(i);
            const ModbusResult result = ModbusResult(0, false);
            resultMap.insert(registerAddr,result);
        }
    }

    _pGuiModel->setCommunicationStats(_pGuiModel->communicationSuccessCount() + success, _pGuiModel->communicationErrorCount() + error);

    emit modbusPollDone(resultMap);
}

void ModbusMaster::closePort(modbus_t *connection)
{
    modbus_close(connection);
    modbus_free(connection);
}


modbus_t * ModbusMaster::openPort(QString ip, quint16 port)
{
    modbus_t * conn;

    conn = modbus_new_tcp(ip.toStdString().c_str(), port);
    if (conn)
    {
        if (modbus_connect(conn) == -1)
        {
#ifdef QT_DEBUG_OUTPUT
            qDebug() << "Connection failed: " << modbus_strerror(errno) << endl;
#endif
            modbus_free(conn);
            conn = 0;
        }
    }
    else
    {
#ifdef QT_DEBUG_OUTPUT
        qDebug() << "New TCP failed: " << modbus_strerror(errno) << endl;
#endif
        conn = 0;
    }

    return conn;
}

qint32 ModbusMaster::readRegisters(modbus_t * pCtx, quint16 startReg, quint32 num, QList<quint16> * pResultList)
{
    qint32 rc = 0;

    quint16 * aRegister = (quint16 *)malloc(num * sizeof(quint16));

    if (modbus_read_registers(pCtx, startReg, num, aRegister) == -1)
    {
        rc = errno;
        qDebug() << "MB: Read failed: " << modbus_strerror(errno) << endl;
    }
    else
    {
        pResultList->clear();
        for (quint32 i = 0; i < num; i++)
        {
            pResultList->append(aRegister[i]);
        }
    }

    free(aRegister);

    return rc;
}
