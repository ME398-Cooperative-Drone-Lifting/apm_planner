/*=====================================================================

QGroundControl Open Source Ground Control Station

(c) 2009-2012 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>

This file is part of the QGROUNDCONTROL project

    QGROUNDCONTROL is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    QGROUNDCONTROL is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with QGROUNDCONTROL. If not, see <http://www.gnu.org/licenses/>.

======================================================================*/

/**
 * @file
 *   @brief Implementation of the waypoint protocol handler
 *
 *   @author Petri Tanskanen <mavteam@student.ethz.ch>
 *
 */

#include "logging.h"
#include "UASWaypointManager.h"
#include "UAS.h"
#include "configuration.h"
#include "MainWindow.h"

#define PROTOCOL_TIMEOUT_MS 2000    ///< maximum time to wait for pending messages until timeout
#define PROTOCOL_DELAY_MS 20        ///< minimum delay between sent messages
#define PROTOCOL_MAX_RETRIES 5      ///< maximum number of send retries (after timeout)

static const QString DEFAULT_REL_ALT = "defaultRelAltitude";

UASWaypointManager::UASWaypointManager(UAS* _uas)
    : uas(_uas),
      current_retries(0),
      current_wp_id(0),
      current_count(0),
      current_state(WP_IDLE),
      current_partner_systemid(0),
      current_partner_compid(MAV_COMP_ID_PRIMARY),
      read_to_edit(false),
      currentWaypointEditable(NULL),
      protocol_timer(this),
      standalone(false),
      uasid(0),
      m_defaultAcceptanceRadius(5.0),
      m_defaultRelativeAlt(0.0),
      waypointIDHandled(65534) // nobody will have a waypoint list with 65534 waypoints.
{
    if (uas)
    {
        uasid = uas->getUASID();
        connect(&protocol_timer, SIGNAL(timeout()), this, SLOT(timeout()));
        connect(uas, SIGNAL(localPositionChanged(UASInterface*,double,double,double,quint64)), this, SLOT(handleLocalPositionChanged(UASInterface*,double,double,double,quint64)));
        connect(uas, SIGNAL(globalPositionChanged(UASInterface*,double,double,double,quint64)), this, SLOT(handleGlobalPositionChanged(UASInterface*,double,double,double,quint64)));
    }
    else
    {
        uasid = 0;
    }

    m_defaultRelativeAlt = readSetting(DEFAULT_REL_ALT, 20.0f).toDouble();

    m_waypointComponentID = QGC::ComponentID();
}

UASWaypointManager::~UASWaypointManager()
{

}

void UASWaypointManager::timeout()
{
    if (current_retries > 0) {
        protocol_timer.start(PROTOCOL_TIMEOUT_MS);
        current_retries--;
        emit updateStatusString(tr("Timeout, retrying (retries left: %1)").arg(current_retries));

        if (current_state == WP_GETLIST) {
            QLOG_WARN() << "Timeout requesting waypoint count - retrying.";
            sendWaypointRequestList();
        } else if (current_state == WP_GETLIST_GETWPS) {
            QLOG_WARN() << "Timeout requesting waypoints - retrying.";
            sendWaypointRequest(current_wp_id);
        } else if (current_state == WP_SENDLIST) {
            QLOG_WARN() << "Timeout sending waypoint count - retrying.";
            sendWaypointCount();
        } else if (current_state == WP_SENDLIST_SENDWPSINT || current_state == WP_SENDLIST_SENDWPSFLOAT) {
            QLOG_WARN() << "Timeout sending waypoints - retrying.";
            sendWaypoint(current_wp_id);
        } else if (current_state == WP_CLEARLIST) {
            QLOG_WARN() << "Timeout sending waypoint clear - retrying.";
            sendWaypointClearAll();
        } else if (current_state == WP_SETCURRENT) {
            QLOG_WARN() << "Timeout sending set current waypoint - retrying.";
            sendWaypointSetCurrent(current_wp_id);
        }
    } else {
        protocol_timer.stop();
        QLOG_WARN() << "Finally timed out - going to idle. Current state was:" << current_state;
        emit updateStatusString("Operation timed out.");

        current_state = WP_IDLE;
        current_count = 0;
        current_wp_id = 0;
        current_partner_systemid = 0;
        current_partner_compid = MAV_COMP_ID_PRIMARY;
    }
}

void UASWaypointManager::handleLocalPositionChanged(UASInterface* mav, double x, double y, double z, quint64 time)
{
    Q_UNUSED(mav);
    Q_UNUSED(time);
    if (waypointsEditable.count() > 0 && currentWaypointEditable && (currentWaypointEditable->getFrame() == MAV_FRAME_LOCAL_NED || currentWaypointEditable->getFrame() == MAV_FRAME_LOCAL_ENU))
    {
        double xdiff = x-currentWaypointEditable->getX();
        double ydiff = y-currentWaypointEditable->getY();
        double zdiff = z-currentWaypointEditable->getZ();
        double dist = sqrt(xdiff*xdiff + ydiff*ydiff + zdiff*zdiff);
        emit waypointDistanceChanged(dist);
    }
}

void UASWaypointManager::handleGlobalPositionChanged(UASInterface* mav, double lat, double lon, double alt, quint64 time)
{
    Q_UNUSED(mav);
    Q_UNUSED(time);
    Q_UNUSED(alt);
    Q_UNUSED(lon);
    Q_UNUSED(lat);
    if (waypointsEditable.count() > 0 && currentWaypointEditable && currentWaypointEditable->isGlobalFrame())
    {
        // TODO FIXME Calculate distance
        double dist = 0;
        emit waypointDistanceChanged(dist);
    }
}

void UASWaypointManager::handleWaypointCount(quint8 systemId, quint8 compId, quint16 count)
{
    if (current_state == WP_GETLIST && systemId == current_partner_systemid) {
        protocol_timer.start(PROTOCOL_TIMEOUT_MS);
        current_retries = PROTOCOL_MAX_RETRIES;

        //Clear the old edit-list before receiving the new one
        if (read_to_edit == true){
            qDeleteAll(waypointsEditable);
            waypointsEditable.clear();
            emit waypointEditableListChanged();
        }

        if (count > 0) {
            current_count = count;
            current_wp_id = 0;
            current_state = WP_GETLIST_GETWPS;
            sendWaypointRequest(current_wp_id);
        } else {
            protocol_timer.stop();
            emit updateStatusString("done.");
            current_state = WP_IDLE;
            current_count = 0;
            current_wp_id = 0;
            current_partner_systemid = 0;
            current_partner_compid = MAV_COMP_ID_PRIMARY;
        }

        QLOG_DEBUG() << "handleWaypointCount() - Number of waypoints to fetch is " << current_count;


    } else {
        QLOG_DEBUG() << "handleWaypointCount() - Rejecting message, check mismatch: current_state: " << current_state
                     << " == " << WP_GETLIST
                     << ", system id " << current_partner_systemid
                     << " == " << systemId
                     << ", comp id " << current_partner_compid
                     << " == "<< compId;
    }
}

// change from mavlink_mission_item_t to mavlink_mission_item_int_t
void UASWaypointManager::handleWaypoint(quint8 systemId, quint8 compId, mavlink_mission_item_int_t *wp)
{
    if (systemId == current_partner_systemid && current_state == WP_GETLIST_GETWPS) {

        if(wp->seq == current_wp_id) {

            protocol_timer.start(PROTOCOL_TIMEOUT_MS);
            current_retries = PROTOCOL_MAX_RETRIES;

            // convert x and y value of waypoints from int32_t to double
            double wp_x = wp->x / (double) 1E7;
            double wp_y = wp->y / (double) 1E7;
            Waypoint *lwp_vo = new Waypoint(wp->seq, wp_x, wp_y, wp->z, wp->param1, wp->param2, wp->param3, wp->param4, wp->autocontinue, wp->current, (MAV_FRAME) wp->frame, (MAV_CMD) wp->command);
            addWaypointViewOnly(lwp_vo);


            if (read_to_edit == true) {
                // using int32_t
                Waypoint *lwp_ed = new Waypoint(wp->seq, wp_x, wp_y, wp->z, wp->param1, wp->param2, wp->param3, wp->param4, wp->autocontinue, wp->current, (MAV_FRAME) wp->frame, (MAV_CMD) wp->command);
                addWaypointEditable(lwp_ed, false);
                if (wp->current == 1) currentWaypointEditable = lwp_ed;
            }

            QLOG_DEBUG() << "handleWaypoint() - Received waypoint " << current_wp_id;
            //get next waypoint
            current_wp_id++;

            if(current_wp_id < current_count) {
                sendWaypointRequest(current_wp_id);
            } else {
                sendWaypointAck(0);

                // all waypoints retrieved, change state to idle
                current_state = WP_IDLE;
                current_count = 0;
                current_wp_id = 0;
                current_partner_systemid = 0;
                current_partner_compid = MAV_COMP_ID_PRIMARY;
                waypointIDHandled = 65534;  // Set to invalid value.

                protocol_timer.stop();
                emit readGlobalWPFromUAS(false);

                QTime time = QTime::currentTime();
                QString timeString = time.toString();
                emit updateStatusString(tr("done. (updated at %1)").arg(timeString));
                QLOG_DEBUG() << "handleWaypoint() - Received all waypoints ";

            }
        } else {
            emit updateStatusString(tr("Waypoint ID mismatch, rejecting waypoint"));
            QLOG_DEBUG() << "handleWaypoint() - Waypoint ID mismatch (expected " << current_wp_id << " got " << wp->seq <<  "), rejecting waypoint for system id " << current_partner_systemid;
        }
    } else {
        QLOG_DEBUG() << "handleWaypoint() - Rejecting message, check mismatch: current_state: " << current_state
                     << " == " << WP_GETLIST_GETWPS
                     << ", system id " << current_partner_systemid
                     << " == " << systemId
                     << ", comp id " << current_partner_compid
                     << " == "<< compId;
    }
}

void UASWaypointManager::handleWaypointAck(quint8 systemId, quint8 compId, mavlink_mission_ack_t *wpa)
{
    if (systemId == current_partner_systemid && (compId == current_partner_compid || compId == MAV_COMP_ID_PRIMARY)) {
        if((current_state == WP_SENDLIST || current_state == WP_SENDLIST_SENDWPSINT || current_state == WP_SENDLIST_SENDWPSFLOAT)
           && (current_wp_id == waypoint_buffer.count()-1 && wpa->type == 0)) {
            //all waypoints sent and ack received
            protocol_timer.stop();
            current_state = WP_IDLE;
            readWaypoints(false); //Update "Onboard Waypoints"-tab immidiately after the waypoint list has been sent.
            emit updateStatusString("done.");
        } else if(current_state == WP_CLEARLIST) {
            protocol_timer.stop();
            current_state = WP_IDLE;
            emit updateStatusString("done.");
        }
    }
}

void UASWaypointManager::handleWaypointRequest(quint8 systemId, quint8 compId, mavlink_mission_request_t *wpr)
{
    handleWaypointRequest(systemId, compId, wpr->seq, MissionItemEncoding::Float);
}

void UASWaypointManager::handleWaypointRequest(quint8 systemId, quint8 compId, mavlink_mission_request_int_t *wpr)
{
    handleWaypointRequest(systemId, compId, wpr->seq, MissionItemEncoding::Int);
}

void UASWaypointManager::handleWaypointRequest(quint8 systemId, quint8 compId, quint16 wpRequestId, MissionItemEncoding wpEncoding)
{
    if (systemId == current_partner_systemid
        && ((current_state == WP_SENDLIST && wpRequestId == 0)
            || ((current_state == WP_SENDLIST_SENDWPSINT || current_state == WP_SENDLIST_SENDWPSFLOAT)
                && (wpRequestId == current_wp_id || wpRequestId == current_wp_id + 1)))
       ) {
        protocol_timer.start(PROTOCOL_TIMEOUT_MS);
        current_retries = PROTOCOL_MAX_RETRIES;

        if (wpRequestId < waypoint_buffer.count()) {
            current_state = wpEncoding == MissionItemEncoding::Int ? WP_SENDLIST_SENDWPSINT : WP_SENDLIST_SENDWPSFLOAT;
            current_wp_id = wpRequestId;
            sendWaypoint(current_wp_id);
        } else {
            QLOG_DEBUG() << "System id:" << current_partner_systemid << "requested waypoint which does not exist."
                         << " Requested waypoint ID:" << wpRequestId << " max waypoint ID:" << waypoint_buffer.size() - 1;
        }
    } else {
        QLOG_DEBUG() << "handleWaypointRequest() - Rejecting message, check mismatch: current_state: " << current_state
                     << " == " << WP_SENDLIST << " or " << WP_SENDLIST_SENDWPSINT << " or " << WP_SENDLIST_SENDWPSFLOAT
                     << ", system id " << current_partner_systemid
                     << " == " << systemId
                     << ", comp id " << current_partner_compid
                     << " == "<< compId;
    }
}

void UASWaypointManager::handleWaypointReached(quint8 systemId, quint8 compId, mavlink_mission_item_reached_t *wpr)
{
    Q_UNUSED(compId);
    if (!uas) return;
    if (systemId == uasid) {
        emit updateStatusString(QString("Reached waypoint %1").arg(wpr->seq));
    }
}

void UASWaypointManager::handleWaypointCurrent(quint8 systemId, quint8 compId, mavlink_mission_current_t *wpc)
{
    Q_UNUSED(compId);
    if (!uas) return;
    if (systemId == uasid) {
        // FIXME Petri
        if (current_state == WP_SETCURRENT) {
            protocol_timer.stop();
            current_state = WP_IDLE;

            // update the local main storage
            if (wpc->seq < waypointsViewOnly.size()) {
                for(int i = 0; i < waypointsViewOnly.size(); i++) {
                    if (waypointsViewOnly[i]->getId() == wpc->seq) {
                        waypointsViewOnly[i]->setCurrent(true);
                    } else {
                        waypointsViewOnly[i]->setCurrent(false);
                    }
                }
            }
        }
        if(waypointIDHandled != wpc->seq)
        {
            waypointIDHandled = wpc->seq;
//            emit updateStatusString(QString("New current waypoint %1").arg(wpc->seq));
            QLOG_INFO() << QString("New current waypoint %1").arg(wpc->seq);
            //emit update to UI widgets
            emit currentWaypointChanged(wpc->seq);
        }
    }
}

void UASWaypointManager::notifyOfChangeEditable(Waypoint* wp)
{
    // If only one waypoint was changed, emit only WP signal
    if (wp != NULL) {
        emit waypointEditableChanged(uasid, wp);
    } else {
        emit waypointEditableListChanged();
        emit waypointEditableListChanged(uasid);
    }
}

void UASWaypointManager::notifyOfChangeViewOnly(Waypoint* wp)
{
    if (wp != NULL) {
        emit waypointViewOnlyChanged(uasid, wp);
    } else {
        emit waypointViewOnlyListChanged();
        emit waypointViewOnlyListChanged(uasid);
    }
}


int UASWaypointManager::setCurrentWaypoint(quint16 seq)
{
    if (seq < waypointsViewOnly.size()) {
        if(current_state == WP_IDLE) {

            //send change to UAS - important to note: if the transmission fails, we have inconsistencies
            protocol_timer.start(PROTOCOL_TIMEOUT_MS);
            current_retries = PROTOCOL_MAX_RETRIES;

            current_state = WP_SETCURRENT;
            current_wp_id = seq;
            current_partner_systemid = uasid;
            current_partner_compid = MAV_COMP_ID_MISSIONPLANNER;

            sendWaypointSetCurrent(current_wp_id);

            return 0;
        }
    }
    return -1;
}

int UASWaypointManager::setCurrentEditable(quint16 seq)
{
    if (seq < waypointsEditable.count()) {
        if(current_state == WP_IDLE) {
            //update local main storage
            for(int i = 0; i < waypointsEditable.count(); i++) {
                if (waypointsEditable[i]->getId() == seq) {
                    waypointsEditable[i]->setCurrent(true);
                } else {
                    waypointsEditable[i]->setCurrent(false);
                }
            }

            return 0;
        }
    }
    return -1;
}

void UASWaypointManager::addWaypointViewOnly(Waypoint *wp)
{
    if (wp)
    {
        waypointsViewOnly.insert(waypointsViewOnly.size(), wp);
        connect(wp, SIGNAL(changed(Waypoint*)), this, SLOT(notifyOfChangeViewOnly(Waypoint*)));

        emit waypointViewOnlyListChanged();
        emit waypointViewOnlyListChanged(uasid);
    }
}


/**
 * @warning Make sure the waypoint stays valid for the whole application lifecycle!
 * @param enforceFirstActive Enforces that the first waypoint is set as active
 * @see createWaypoint() is more suitable for most use cases
 */
void UASWaypointManager::addWaypointEditable(Waypoint *wp, bool enforceFirstActive)
{
    if (wp)
    {
        // Check if this is the first waypoint in an offline list
        if (waypointsEditable.count() == 0 && uas == NULL)
            MainWindow::instance()->showCriticalMessage(tr("OFFLINE Waypoint Editing Mode"), tr("You are in offline editing mode. Make sure to save your mission to a file before connecting to a system - you will need to load the file into the system, the offline list will be cleared on connect."));

        wp->setId(waypointsEditable.count());
        if (enforceFirstActive && waypointsEditable.count() == 0)
        {
            wp->setCurrent(true);
            currentWaypointEditable = wp;
        }
        waypointsEditable.insert(waypointsEditable.count(), wp);
        connect(wp, SIGNAL(changed(Waypoint*)), this, SLOT(notifyOfChangeEditable(Waypoint*)));

        // Moved to caller - if all waypoints are received.
        emit waypointEditableListChanged();
        emit waypointEditableListChanged(uasid);
    }
}

/**
 * @param enforceFirstActive Enforces that the first waypoint is set as active
 */
Waypoint* UASWaypointManager::createWaypoint(bool enforceFirstActive)
{
    // Check if this is the first waypoint in an offline list
    if (waypointsEditable.count() == 0 && uas == NULL)
        MainWindow::instance()->showCriticalMessage(tr("OFFLINE Waypoint Editing Mode"), tr("You are in offline editing mode. Make sure to save your mission to a file before connecting to a system - you will need to load the file into the system, the offline list will be cleared on connect."));

    Waypoint* wp = new Waypoint();
    wp->setId(waypointsEditable.count());
    wp->setFrame((MAV_FRAME)getFrameRecommendation());
    wp->setAltitude(getAltitudeRecommendation(wp->getFrame()));
    wp->setAcceptanceRadius(getAcceptanceRadiusRecommendation());
    if (enforceFirstActive && waypointsEditable.count() == 0)
    {
        wp->setCurrent(true);
        currentWaypointEditable = wp;
    }
    waypointsEditable.append(wp);
    connect(wp, SIGNAL(changed(Waypoint*)), this, SLOT(notifyOfChangeEditable(Waypoint*)));

    emit waypointEditableListChanged();
    emit waypointEditableListChanged(uasid);
    return wp;
}

int UASWaypointManager::removeWaypoint(quint16 seq)
{
    if (seq < waypointsEditable.count())
    {
        Waypoint *t = waypointsEditable[seq];

        if (t->getCurrent() == true) //trying to remove the current waypoint
        {
            if (seq+1 < waypointsEditable.count()) // setting the next waypoint as current
            {
                waypointsEditable[seq+1]->setCurrent(true);
            }
            else if (seq-1 >= 0) // if deleting the last on the list, then setting the previous waypoint as current
            {
                waypointsEditable[seq-1]->setCurrent(true);
            }
        }

        waypointsEditable.removeAt(seq);
        delete t;
        t = NULL;

        for(int i = seq; i < waypointsEditable.count(); i++)
        {
            waypointsEditable[i]->setId(i);
        }

        emit waypointEditableListChanged();
        emit waypointEditableListChanged(uasid);
        return 0;
    }
    return -1;
}

void UASWaypointManager::moveWaypoint(quint16 cur_seq, quint16 new_seq)
{
    if (cur_seq != new_seq && cur_seq < waypointsEditable.count() && new_seq < waypointsEditable.count())
    {
        Waypoint *t = waypointsEditable[cur_seq];
        if (cur_seq < new_seq) {
            for (int i = cur_seq; i < new_seq; i++)
            {
                waypointsEditable[i] = waypointsEditable[i+1];
                waypointsEditable[i]->setId(i);
            }
        }
        else
        {
            for (int i = cur_seq; i > new_seq; i--)
            {
                waypointsEditable[i] = waypointsEditable[i-1];
                waypointsEditable[i]->setId(i);
            }
        }
        waypointsEditable[new_seq] = t;
        waypointsEditable[new_seq]->setId(new_seq);

        emit waypointEditableListChanged();
        emit waypointEditableListChanged(uasid);
    }
}

void UASWaypointManager::saveWaypoints(const QString &saveFile)
{
    QFile file(saveFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return;

    QTextStream out(&file);

    //write the waypoint list version to the first line for compatibility check
    out << "QGC WPL 110\r\n";

    for (int i = 0; i < waypointsEditable.count(); i++)
    {
        waypointsEditable[i]->setId(i);
        waypointsEditable[i]->save(out);
    }
    file.close();
}

void UASWaypointManager::loadWaypoints(const QString &loadFile)
{
    QFile file(loadFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    qDeleteAll(waypointsEditable);
    waypointsEditable.clear();

    emit waypointEditableListChanged();
    emit waypointEditableListChanged(uasid);

    QTextStream in(&file);

    const QStringList &version = in.readLine().split(" ");

    if (version.length() < 2){
        emit updateStatusString(tr("Waypoint file is corrupt. Version not detectable"));
        return;
    }

    int versionInt = version[2].toInt();

    if (!(version.size() == 3 && version[0] == "QGC" && version[1] == "WPL" && versionInt >= 110))
    {
        emit updateStatusString(tr("The waypoint file is version %1 and is not compatible").arg(versionInt));
    }
    else
    {
        while (!in.atEnd())
        {
            Waypoint *t = new Waypoint();
            if(t->load(in))
            {
                t->setId(waypointsEditable.count());
                waypointsEditable.insert(waypointsEditable.count(), t);
            }
            else
            {
                emit updateStatusString(tr("The waypoint file is corrupted. Load operation only partly succesful."));
                break;
            }
        }
    }

    file.close();

    emit loadWPFile();
    emit waypointEditableListChanged();
    emit waypointEditableListChanged(uasid);
}

void UASWaypointManager::clearWaypointList()
{
    if (current_state == WP_IDLE)
    {
        protocol_timer.start(PROTOCOL_TIMEOUT_MS);
        current_retries = PROTOCOL_MAX_RETRIES;

        current_state = WP_CLEARLIST;
        current_wp_id = 0;
        current_partner_systemid = uasid;
        current_partner_compid = m_waypointComponentID;

        sendWaypointClearAll();
    }
}

const QList<Waypoint *> UASWaypointManager::getGlobalFrameWaypointList()
{
    // TODO Keep this global frame list up to date
    // with complete waypoint list
    // instead of filtering on each request
    QList<Waypoint*> wps;
    foreach (Waypoint* wp, waypointsEditable)
    {
        if (wp->isGlobalFrame())
        {
            wps.append(wp);
        }
    }
    return wps;
}

const QList<Waypoint *> UASWaypointManager::getGlobalFrameAndNavTypeWaypointList(bool onlypath)
{
    // TODO Keep this global frame list up to date
    // with complete waypoint list
    // instead of filtering on each request
    QList<Waypoint*> wps;
    foreach (Waypoint* wp, waypointsEditable)
    {
        if ((wp->isGlobalFrame()) && (wp->isNavigationType() || (wp->visibleOnMapWidget())))
        {
            if(wp->visibleOnMapWidget() && onlypath) // we need waypoints only to draw the path on map
                continue;
            wps.append(wp);
        }
    }
    return wps;
}

const QList<Waypoint *> UASWaypointManager::getNavTypeWaypointList()
{
    // TODO Keep this global frame list up to date
    // with complete waypoint list
    // instead of filtering on each request
    QList<Waypoint*> wps;
    foreach (Waypoint* wp, waypointsEditable)
    {
        if (wp->isNavigationType())
        {
            wps.append(wp);
        }
    }
    return wps;
}

int UASWaypointManager::getIndexOf(Waypoint* wp)
{
    return waypointsEditable.indexOf(wp);
}

int UASWaypointManager::getGlobalFrameIndexOf(Waypoint* wp)
{
    // Search through all waypointsEditable,
    // counting only those in global frame
    int i = 0;
    foreach (Waypoint* p, waypointsEditable) {
        if (p->isGlobalFrame())
        {
            if (p == wp)
            {
                return i;
            }
            i++;
        }
    }

    return -1;
}

int UASWaypointManager::getGlobalFrameAndNavTypeIndexOf(Waypoint* wp)
{
    // Search through all waypointsEditable,
    // counting only those in global frame
    int i = 0;
    foreach (Waypoint* p, waypointsEditable) {
        if (p->isGlobalFrame() && p->isNavigationType())
        {
            if (p == wp)
            {
                return i;
            }
            i++;
        }
    }

    return -1;
}

int UASWaypointManager::getNavTypeIndexOf(Waypoint* wp)
{
    // Search through all waypointsEditable,
    // counting only those in global frame
    int i = 0;
    foreach (Waypoint* p, waypointsEditable)
    {
        if (p->isNavigationType())
        {
            if (p == wp)
            {
                return i;
            }
            i++;
        }
    }

    return -1;
}

int UASWaypointManager::getGlobalFrameCount()
{
    // Search through all waypointsEditable,
    // counting only those in global frame
    int i = 0;
    foreach (Waypoint* p, waypointsEditable)
    {
        if (p->isGlobalFrame())
        {
            i++;
        }
    }

    return i;
}

int UASWaypointManager::getGlobalFrameAndNavTypeCount()
{
    // Search through all waypointsEditable,
    // counting only those in global frame
    int i = 0;
    foreach (Waypoint* p, waypointsEditable) {
        if (p->isGlobalFrame() && p->isNavigationType())
        {
            i++;
        }
    }

    return i;
}

int UASWaypointManager::getNavTypeCount()
{
    // Search through all waypointsEditable,
    // counting only those in global frame
    int i = 0;
    foreach (Waypoint* p, waypointsEditable) {
        if (p->isNavigationType()) {
            i++;
        }
    }

    return i;
}

int UASWaypointManager::getLocalFrameCount()
{
    // Search through all waypointsEditable,
    // counting only those in global frame
    int i = 0;
    foreach (Waypoint* p, waypointsEditable)
    {
        if (p->getFrame() == MAV_FRAME_LOCAL_NED || p->getFrame() == MAV_FRAME_LOCAL_ENU)
        {
            i++;
        }
    }

    return i;
}

int UASWaypointManager::getLocalFrameIndexOf(Waypoint* wp)
{
    // Search through all waypointsEditable,
    // counting only those in local frame
    int i = 0;
    foreach (Waypoint* p, waypointsEditable)
    {
        if (p->getFrame() == MAV_FRAME_LOCAL_NED || p->getFrame() == MAV_FRAME_LOCAL_ENU)
        {
            if (p == wp)
            {
                return i;
            }
            i++;
        }
    }

    return -1;
}

int UASWaypointManager::getMissionFrameIndexOf(Waypoint* wp)
{
    // Search through all waypointsEditable,
    // counting only those in mission frame
    int i = 0;
    foreach (Waypoint* p, waypointsEditable)
    {
        if (p->getFrame() == MAV_FRAME_MISSION)
        {
            if (p == wp)
            {
                return i;
            }
            i++;
        }
    }

    return -1;
}


/**
 * @param readToEdit If true, incoming waypoints will be copied both to "edit"-tab and "view"-tab. Otherwise, only to "view"-tab.
 */
void UASWaypointManager::readWaypoints(bool readToEdit)
{
    read_to_edit = readToEdit;
    emit readGlobalWPFromUAS(true);
    if(current_state == WP_IDLE) {


        //Clear the old view-list before receiving the new one
        qDeleteAll(waypointsViewOnly);
        waypointsViewOnly.clear();
        emit waypointViewOnlyListChanged();
        /* THIS PART WAS MOVED TO handleWaypointCount. THE EDIT-LIST SHOULD NOT BE CLEARED UNLESS THERE IS A RESPONSE FROM UAV.
        //Clear the old edit-list before receiving the new one
        if (read_to_edit == true){
            while(waypointsEditable.count()>0) {
                Waypoint *t = waypointsEditable[0];
                waypointsEditable.remove(0);
                delete t;
            }
            emit waypointEditableListChanged();
        }
        */
        protocol_timer.start(PROTOCOL_TIMEOUT_MS);
        current_retries = PROTOCOL_MAX_RETRIES;

        current_state = WP_GETLIST;
        current_wp_id = 0;
        current_partner_systemid = uasid;
        current_partner_compid = m_waypointComponentID;

        sendWaypointRequestList();

    }
}
bool UASWaypointManager::guidedModeSupported()
{
    return (uas->getAutopilotType() == MAV_AUTOPILOT_ARDUPILOTMEGA);
}

// change mavlink_mission_item_t to mavlink_mission_item_int_t
void UASWaypointManager::goToWaypoint(Waypoint *wp)
{
    if (!uas) return;

    //Don't try to send a guided mode message to an AP that does not support it.
    if (uas->getAutopilotType() == MAV_AUTOPILOT_ARDUPILOTMEGA)
    {
        QLOG_DEBUG() << "APM: goToWaypont: " + wp->debugString();
        mavlink_mission_item_int_t mission;
        memset(&mission, 0, sizeof(mavlink_mission_item_int_t));   //initialize with zeros
        //const Waypoint *cur_s = waypointsEditable.at(i);

        mission.autocontinue = 0;
        mission.current = 2; //2 for guided mode
        mission.param1 = wp->getParam1();
        mission.param2 = wp->getParam2();
        mission.param3 = wp->getParam3();
        mission.param4 = wp->getParam4();
        mission.frame = wp->getFrame();
        mission.command = wp->getAction();
        mission.seq = 0;     // don't read out the sequence number of the waypoint class
        // convert fromt double to int32_t
        mission.x = (int32_t) (wp->getX() * 1E7);
        mission.y = (int32_t) (wp->getY() * 1E7);
        mission.z = wp->getZ();
        mavlink_message_t message;
        mission.target_system = uasid;
        mission.target_component = m_waypointComponentID;
        //using mavlink_msg_mission_item_int_encode to encode mavlink_mission_item_int_t type message
        mavlink_msg_mission_item_int_encode(uas->getSystemId(), uas->getComponentId(), &message, &mission);
        uas->sendMessage(message);
        QGC::SLEEP::msleep(PROTOCOL_DELAY_MS);
    }
}

// change mavlink_mission_item_t to mavlink_mission_item_int_t
void UASWaypointManager::writeWaypoints()
{
    if (current_state == WP_IDLE) {
        // Send clear all if count == 0
        if (waypointsEditable.count() > 0) {
            protocol_timer.start(PROTOCOL_TIMEOUT_MS);
            current_retries = PROTOCOL_MAX_RETRIES;

            current_count = waypointsEditable.count();
            current_state = WP_SENDLIST;
            current_wp_id = 0;
            current_partner_systemid = uasid;
            current_partner_compid = m_waypointComponentID;

            //clear local buffer
            // Why not replace with waypoint_buffer.clear() ?
            // because this will lead to memory leaks, the waypoint-structs
            // have to be deleted, clear() would only delete the pointers.
            while(!waypoint_buffer.empty()) {
                delete waypoint_buffer.back();
                waypoint_buffer.pop_back();
            }

            bool noCurrent = true;

            //copy waypoint data to local buffer
            for (int i=0; i < current_count; i++) {
                waypoint_buffer.push_back(new mavlink_mission_item_int_t);
                mavlink_mission_item_int_t *cur_d = waypoint_buffer.back();
                memset(cur_d, 0, sizeof(mavlink_mission_item_int_t));   //initialize with zeros
                const Waypoint *cur_s = waypointsEditable.at(i);

                cur_d->autocontinue = cur_s->getAutoContinue();
                cur_d->current = cur_s->getCurrent() & noCurrent;   //make sure only one current waypoint is selected, the first selected will be chosen
                cur_d->param1 = cur_s->getParam1();
                cur_d->param2 = cur_s->getParam2();
                cur_d->param3 = cur_s->getParam3();
                cur_d->param4 = cur_s->getParam4();
                cur_d->frame = cur_s->getFrame();
                cur_d->command = cur_s->getAction();
                cur_d->seq = i;     // don't read out the sequence number of the waypoint class
                // convert fromt double to int32_t
                cur_d->x = (int32_t) (cur_s->getX() * 1E7);
                cur_d->y = (int32_t) (cur_s->getY() * 1E7);
                cur_d->z = cur_s->getZ();

                if (cur_s->getCurrent() && noCurrent)
                    noCurrent = false;
                if (i == (current_count - 1) && noCurrent == true) //not a single waypoint was set as "current"
                    cur_d->current = true; // set the last waypoint as current. Or should it better be the first waypoint ?
            }




            //send the waypoint count to UAS (this starts the send transaction)
            sendWaypointCount();
        } else if (waypointsEditable.count() == 0)
        {
            sendWaypointClearAll();
        }
    }
    else
    {
        //we're in another transaction, ignore command
        QLOG_DEBUG() << "UASWaypointManager::sendWaypoints() doing something else ignoring command";
    }
}

void UASWaypointManager::sendWaypointClearAll()
{
    if (!uas) return;
    mavlink_message_t message;
    mavlink_mission_clear_all_t wpca;

    wpca.target_system = uasid;
    wpca.target_component = m_waypointComponentID;

    emit updateStatusString(QString("Clearing waypoint list..."));

    mavlink_msg_mission_clear_all_encode(uas->getSystemId(), uas->getComponentId(), &message, &wpca);

    uas->sendMessage(message);
    QGC::SLEEP::msleep(PROTOCOL_DELAY_MS);
}

void UASWaypointManager::sendWaypointSetCurrent(quint16 seq)
{
    if (!uas) return;
    mavlink_message_t message;
    mavlink_mission_set_current_t wpsc;

    wpsc.target_system = uasid;
    wpsc.target_component = m_waypointComponentID;
    wpsc.seq = seq;

    emit updateStatusString(QString("Updating target waypoint..."));

    mavlink_msg_mission_set_current_encode(uas->getSystemId(), uas->getComponentId(), &message, &wpsc);
    uas->sendMessage(message);
    QGC::SLEEP::msleep(PROTOCOL_DELAY_MS);
}

void UASWaypointManager::sendWaypointCount()
{
    if (!uas) return;
    mavlink_message_t message;
    mavlink_mission_count_t wpc;

    wpc.target_system = uasid;
    wpc.target_component = m_waypointComponentID;
    wpc.count = current_count;
    wpc.mission_type = MAV_MISSION_TYPE_MISSION;

    emit updateStatusString(QString("Starting to transmit waypoints..."));

    mavlink_msg_mission_count_encode(uas->getSystemId(), uas->getComponentId(), &message, &wpc);
    uas->sendMessage(message);
    QGC::SLEEP::msleep(PROTOCOL_DELAY_MS);
}

void UASWaypointManager::sendWaypointRequestList()
{
    if (!uas) return;
    mavlink_message_t message;
    mavlink_mission_request_list_t wprl;

    wprl.target_system = uasid;
    wprl.target_component = m_waypointComponentID;

    emit updateStatusString(QString("Requesting waypoint list..."));

    mavlink_msg_mission_request_list_encode(uas->getSystemId(), uas->getComponentId(), &message, &wprl);
    uas->sendMessage(message);
    QGC::SLEEP::msleep(PROTOCOL_DELAY_MS);
}

// request int32_t for wp gps position
// change mavlink_mission_request_t to mavlink_mission_request_int_t
void UASWaypointManager::sendWaypointRequest(quint16 seq)
{
    if (!uas) return;
    mavlink_message_t message;
    mavlink_mission_request_int_t wpr;

    wpr.target_system = uasid;
    wpr.target_component = m_waypointComponentID;
    wpr.seq = seq;

    emit updateStatusString(QString("Retrieving waypoint ID %1 of %2 total").arg(wpr.seq).arg(current_count));

    //using mavlink_msg_mission_request_int_encode to encode mavlink_mission_request_int_t type message
    mavlink_msg_mission_request_int_encode(uas->getSystemId(), uas->getComponentId(), &message, &wpr);
    uas->sendMessage(message);
    QGC::SLEEP::msleep(PROTOCOL_DELAY_MS);
}

// change mavlink_mission_item_t to mavlink_mission_item_int_t
void UASWaypointManager::sendWaypoint(quint16 seq)
{
    if (!uas) return;
    mavlink_message_t message;

    if (seq < waypoint_buffer.count()) {

        mavlink_mission_item_int_t *wp;

        wp = waypoint_buffer.at(seq);
        wp->target_system = uasid;
        wp->target_component = m_waypointComponentID;

        if (current_state == WP_SENDLIST_SENDWPSINT) {
            mavlink_msg_mission_item_int_encode(uas->getSystemId(), uas->getComponentId(), &message, wp);
        } else if (current_state == WP_SENDLIST_SENDWPSFLOAT) {
            mavlink_mission_item_t wp_float;
            convertMavlinkMissionItem(wp, &wp_float);
            mavlink_msg_mission_item_encode(uas->getSystemId(), uas->getComponentId(), &message, &wp_float);
        }
        else {
            QLOG_DEBUG() << "sendWaypoint() - Current state does not allow sending waypoints. Check failed: current_state: " << current_state
                         << " == " << WP_SENDLIST_SENDWPSINT << " or " << WP_SENDLIST_SENDWPSFLOAT;
            return;
        }

        emit updateStatusString(QString("Sending waypoint ID %1 of %2 total").arg(wp->seq).arg(current_count));
        uas->sendMessage(message);
        QGC::SLEEP::msleep(PROTOCOL_DELAY_MS);
    }
}

void UASWaypointManager::sendWaypointAck(quint8 type)
{
    if (!uas) return;
    mavlink_message_t message;
    mavlink_mission_ack_t wpa;

    wpa.target_system = uasid;
    wpa.target_component = m_waypointComponentID;
    wpa.type = type;

    mavlink_msg_mission_ack_encode(uas->getSystemId(), uas->getComponentId(), &message, &wpa);
    uas->sendMessage(message);
    QGC::SLEEP::msleep(PROTOCOL_DELAY_MS);
}

void UASWaypointManager::convertMavlinkMissionItem(mavlink_mission_item_int_t *from, mavlink_mission_item_t *to)
{
    to->target_system = from->target_system;
    to->target_component = from->target_component;
    to->seq = from->seq;
    to->frame = from->frame;
    to->command = from->command;
    to->current = from->current;
    to->autocontinue = from->autocontinue;
    to->param1 = from->param1;
    to->param2 = from->param2;
    to->param3 = from->param3;
    to->param4 = from->param4;
    to->x = 1e-7 * (double)from->x; // only applies to global frames, local frames are scaled by 1e4
    to->y = 1e-7 * (double)from->y;
    to->z = from->z; // this is a float in both cases
    to->mission_type = from->mission_type;
}

UAS* UASWaypointManager::getUAS() {
    return this->uas;    ///< Returns the owning UAS
}

double UASWaypointManager::getAltitudeRecommendation(MAV_FRAME frame)
{
    if (frame == MAV_FRAME_GLOBAL) {
        if (waypointsEditable.count() == 1)
            return waypointsEditable.last()->getAltitude() + m_defaultRelativeAlt;
        else if (waypointsEditable.count() > 1)
            return waypointsEditable.last()->getAltitude();
        else
            return 0.0f; // This returns 0.0m for NAV: Home

    } else { // working in the relative frame
        if (waypointsEditable.count() == 1)
            return m_defaultRelativeAlt;
        else if (waypointsEditable.count() > 1)
            return waypointsEditable.last()->getAltitude();
        else
            return 0.0f; // This returns 0.0m for NAV: Home
    }
}

void UASWaypointManager::setDefaultRelAltitude(double alt)
{
    m_defaultRelativeAlt = alt;
    writeSetting(DEFAULT_REL_ALT, m_defaultRelativeAlt);
}

double UASWaypointManager::getDefaultRelAltitude()
{
    return m_defaultRelativeAlt;
}

int UASWaypointManager::getFrameRecommendation()
{
    // APM always uses waypoint 0 as HOME location (ie. it's MAV_FRAME_GLOBAL)
    if (!uas) {
        // Offline Edit mode.
        if (waypointsEditable.count() == 0){ // Home is always ABS ALT.
            return MAV_FRAME_GLOBAL;
        } else if (waypointsEditable.count() > 1) {
            // new waypoints adopt the last waypoint setting by default.
            return static_cast<int>(waypointsEditable.last()->getFrame());
        } else {
            // Always make WP1 in offline mode relative
            return MAV_FRAME_GLOBAL_RELATIVE_ALT;
        }
    }

    // Online edit rules
    if (waypointsEditable.count() > 1) {
        // new waypoints adopt the last waypoint setting by default.
        return static_cast<int>(waypointsEditable.last()->getFrame());
    } else {
        // Always make WP1 relative
        return MAV_FRAME_GLOBAL_RELATIVE_ALT;
    }
}

double UASWaypointManager::getAcceptanceRadiusRecommendation()
{
    if (waypointsEditable.count() > 0) {
        return waypointsEditable.last()->getAcceptanceRadius();
    } else {
        return m_defaultAcceptanceRadius;
    }
}

const Waypoint *UASWaypointManager::getWaypoint(int index)
{
    if (index > 0 || index < waypointsEditable.count()){
        return waypointsEditable[index];
    } else {
        return NULL;
    }
}

void UASWaypointManager::writeSetting(const QString &key, const QVariant &value)
{
    QSettings settings;
    settings.beginGroup("WAYPOINT_MANAGER");
    settings.setValue(key,value);
    settings.endGroup();
    settings.sync();
}

const QVariant UASWaypointManager::readSetting(const QString& key, const QVariant& defaultValue)
{
    QSettings settings;
    settings.beginGroup("WAYPOINT_MANAGER");
    QVariant result = settings.value(key, defaultValue);
    settings.endGroup();
    settings.sync();
    return result;
}

