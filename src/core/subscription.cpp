/******************************************************************************
 *   Copyright (C) 2014-2014 Olivier Roulet-Dubonnet          *
 *   olivier.roulet@gmail.com          *
 *                      *
 *   This library is free software; you can redistribute it and/or modify   *
 *   it under the terms of the GNU Lesser General Public License as      *
 *   published by the Free Software Foundation; version 3 of the License.   *
 *                      *
 *   This library is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of      *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the      *
 *   GNU Lesser General Public License for more details.        *
 *                      *
 *   You should have received a copy of the GNU Lesser General Public License *
 *   along with this library; if not, write to the          *
 *   Free Software Foundation, Inc.,              *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.        *
 ******************************************************************************/


#include <opc/ua/subscription.h>
#include <opc/ua/protocol/string_utils.h>

#include <boost/asio.hpp>
#include <iostream>

namespace OpcUa
{
  Subscription::Subscription(Services::SharedPtr server, const SubscriptionParameters& params, SubscriptionHandler& callback, bool debug)
    : Server(server), Client(callback), Debug(debug)
  {
    CreateSubscriptionRequest request;
    request.Parameters = params;
    Services::SharedPtr serverptr = Server;
    Data = Server->Subscriptions()->CreateSubscription(request, [this, serverptr](PublishResult i){ this->PublishCallback(serverptr, i); } );
    //After creating the subscription, it is expected to send at least one publish request
    Server->Subscriptions()->Publish(PublishRequest());
    Server->Subscriptions()->Publish(PublishRequest());
  }

  void Subscription::Delete()
  {
    std::vector<StatusCode> results = Server->Subscriptions()->DeleteSubscriptions(std::vector<IntegerID>({Data.ID}));
    for (auto res: results)
    {
      CheckStatusCode(res);
    }
  }

  void Subscription::PublishCallback(Services::SharedPtr server, const PublishResult result)
  {

    if (Debug){ std::cout << "Subscription | Suscription::PublishCallback called with " <<result.Message.Data.size() << " notifications " << std::endl; }
    for (const NotificationData& data: result.Message.Data )
    {
      if (data.Header.TypeID == ExpandedObjectID::DataChangeNotification)
      {
        if (Debug) { std::cout << "Subscription | Notification is of type DataChange\n"; }
        CallDataChangeCallback(data);
      }
      else if (data.Header.TypeID == ExpandedObjectID::EventNotificationList)
      {
        if (Debug) { std::cout << "Subscription | Notification is of type Event\n"; }
        CallEventCallback(data);
      }
      else if (data.Header.TypeID == ExpandedObjectID::StatusChangeNotification)
      {
        if (Debug) { std::cout << "Subscription | Notification is of type StatusChange\n"; }
        CallStatusChangeCallback(data);
      }
      else
      {
        std::cout << "Subscription | Error unknown notficiation type received: " << data.Header.TypeID <<std::endl;
      }
    }
    OpcUa::SubscriptionAcknowledgement ack;
    ack.SubscriptionID = GetId();
    ack.SequenceNumber = result.Message.SequenceID;
    PublishRequest request;
    request.Parameters.Acknowledgements.push_back(ack);
    server->Subscriptions()->Publish(request);
  }

  void Subscription::CallDataChangeCallback(const NotificationData& data)
  {
    for ( const MonitoredItems& item: data.DataChange.Notification)
    {
      std::unique_lock<std::mutex> lock(Mutex); //could used boost::shared_lock to improve perf

      AttValMap::iterator mapit = AttributeValueMap.find(item.ClientHandle);
      if ( mapit == AttributeValueMap.end() )
      {
        std::cout << "Subscription | Server Error got publishresult for an unknown  monitoreditem id : "<< item.ClientHandle << std::endl; 
      }
      else
      {
        AttributeID attr = mapit->second.Attribute;
        Node node = mapit->second.TargetNode;
        lock.unlock(); //unlock before calling client cades, you never know what they may do
        if (Debug) { std::cout << "Subscription | Debug: Calling DataChange user callback " << item.ClientHandle << " and node: " << mapit->second.TargetNode << std::endl; }
        Client.DataChange(mapit->second.MonitoredItemID, node, item.Value.Value, attr);
      }
    }
  }

  void Subscription::CallStatusChangeCallback(const NotificationData& data)
  {
     Client.StatusChange(data.StatusChange.Status);
  }

  void Subscription::CallEventCallback(const NotificationData& data)
  {
    for ( EventFieldList ef :  data.Events.Events)
    {
      std::unique_lock<std::mutex> lock(Mutex); //could used boost::shared_lock to improve perf

      AttValMap::iterator mapit = AttributeValueMap.find(ef.ClientHandle);
      if ( mapit == AttributeValueMap.end() )
      {
        std::cout << "Subscription | Server Error got publishresult for an unknown  monitoreditem id : "<< ef.ClientHandle << std::endl; 
      }
      else
      {
        Event ev;
        uint32_t count = 0;
        if ( mapit->second.Filter.Event.SelectClauses.size() != ef.EventFields.size() )
        {
          throw std::runtime_error("Subscription | Error receive event format does not match requested filter");
        }
        for (SimpleAttributeOperand op : mapit->second.Filter.Event.SelectClauses )
        {
          //set the default fiedls of events into their event attributes
          if ( op.BrowsePath.size() == 1 )
          {
            if ( op.BrowsePath[0] == QualifiedName("EventId", 0) )
            {
              ev.EventId = ef.EventFields[count].As<ByteString>();
            }
            else if ( op.BrowsePath[0] == QualifiedName("EventType", 0) )
            {
              ev.EventType = ef.EventFields[count].As<NodeID>();
            }
            else if ( op.BrowsePath[0] == QualifiedName("SourceNode", 0) )
            {
              ev.SourceNode = ef.EventFields[count].As<NodeID>();
            }
            else if ( op.BrowsePath[0] == QualifiedName("SourceName", 0) )
            {
              ev.SourceName = ef.EventFields[count].As<std::string>();
            }
            else if ( op.BrowsePath[0] == QualifiedName("Message", 0) )
            {
              ev.Message = ef.EventFields[count].As<LocalizedText>();
            }
            else if ( op.BrowsePath[0] == QualifiedName("Severity", 0) )
            {
              ev.Severity = ef.EventFields[count].As<uint16_t>();
            }
            else if ( op.BrowsePath[0] == QualifiedName("LocalTime", 0) )
            {
              ev.LocalTime = ef.EventFields[count].As<DateTime>();
            }
            else if ( op.BrowsePath[0] == QualifiedName("ReceiveTime", 0) )
            {
              ev.ReceiveTime = ef.EventFields[count].As<DateTime>();
            }
            else if ( op.BrowsePath[0] == QualifiedName("Time", 0) )
            {
              ev.Time = ef.EventFields[count].As<DateTime>();
            }
          }
          //Add anyway all fields as value
          ev.SetValue(op.BrowsePath, ef.EventFields[count]);
          ++count;
        }
        lock.unlock(); 
        if (Debug) { std::cout << "Subscription | Debug: Calling client event callback\n"; }
        Client.Event(mapit->second.MonitoredItemID, ev);
        if (Debug) { std::cout << "Subscription | Debug: callback call finished\n"; }
      }
    }
  }

  RepublishResponse Subscription::Republish(uint32_t sequenceNumber)
  {
    RepublishParameters params;
    params.Subscription = Data.ID;
    params.Counter = sequenceNumber;
    RepublishResponse response = Server->Subscriptions()->Republish(params);
    return response;
  }

  uint32_t Subscription::SubscribeDataChange(const Node& node, AttributeID attr)
  {
    AttributeValueID avid;
    avid.Node = node.GetId();
    avid.Attribute = attr;
    //avid.IndexRange //We leave it null, then the entire array is returned
    std::vector<uint32_t> results = SubscribeDataChange(std::vector<AttributeValueID>({avid}));
    if (results.size() != 1) { throw std::runtime_error("Subscription | Server error, SubscribeDataChange should have returned exactly one result"); }
    return results.front();
  }

  std::vector<uint32_t> Subscription::SubscribeDataChange(const std::vector<AttributeValueID>& attributes)
  {
    std::unique_lock<std::mutex> lock(Mutex); 

    MonitoredItemsParameters itemsParams;
    itemsParams.SubscriptionID = Data.ID;

    for (AttributeValueID attr : attributes)
    {
      MonitoredItemRequest req;
      req.ItemToMonitor = attr;
      req.Mode = MonitoringMode::Reporting;
      MonitoringParameters params;
      params.SamplingInterval = Data.RevisedPublishingInterval;
      params.QueueSize = 1;
      params.DiscardOldest = true;
      params.ClientHandle = IntegerID(++LastMonitoredItemHandle);
      req.Parameters = params;
      itemsParams.ItemsToCreate.push_back(req);
    }

    std::vector<CreateMonitoredItemsResult> results =  Server->Subscriptions()->CreateMonitoredItems(itemsParams).Results;

    if ( results.size() != attributes.size() ) 
    {
      throw(std::runtime_error("Subscription | Error server did not send answer for all monitoreditem requests"));
    }

    std::vector<uint32_t> monitoredItemsIds;
    unsigned int i = 0;
    for (const auto& res : results)
    {
      CheckStatusCode(res.Status);
      if (Debug ) { std::cout << "Subscription | storing monitoreditem with handle " << itemsParams.ItemsToCreate[i].Parameters.ClientHandle << " and id " << res.MonitoredItemID << std::endl;  }
      MonitoredItemData mdata; 
      mdata.MonitoredItemID = res.MonitoredItemID;
      mdata.Attribute =  attributes[i].Attribute;
      mdata.TargetNode =  Node(Server, attributes[i].Node);
      AttributeValueMap[itemsParams.ItemsToCreate[i].Parameters.ClientHandle] = mdata;
      monitoredItemsIds.push_back(res.MonitoredItemID);
      ++i;
    }
    return monitoredItemsIds;
  }

  void Subscription::UnSubscribe(uint32_t handle)
  {
    return UnSubscribe(std::vector<uint32_t>(1, handle));
  }

  void Subscription::UnSubscribe(std::vector<uint32_t> handles) 
  {
    std::unique_lock<std::mutex> lock(Mutex); 

    DeleteMonitoredItemsParameters params;
    params.SubscriptionId = Data.ID;
    std::vector<IntegerID> mids;
    for (auto id : handles)
    {
      if (Debug) std::cout << "Subscription | Sending unsubscribe for monitoreditemsid: " << id << std::endl;
      mids.push_back(IntegerID(id));
      //Now trying to remove monitoreditem from our internal cache
      for ( auto pair : AttributeValueMap )
      {
        if (pair.second.MonitoredItemID == id)
        {
          AttributeValueMap.erase(pair.first);
          break; //we modified our iterating object, so quit!!
        }
      }
    }
    params.MonitoredItemsIds = mids;
    auto results = Server->Subscriptions()-> DeleteMonitoredItems(params);
    for (auto res : results)
    {
      CheckStatusCode(res);
    }
  }

  uint32_t Subscription::SubscribeEvents()
  {
    return SubscribeEvents(Node(Server, ObjectID::Server), Node(Server, ObjectID::BaseEventType));
  }

  uint32_t Subscription::SubscribeEvents(const Node& node, const Node& eventtype)  
  {
    EventFilter filter;
    if (Debug) std::cout << "Subscription | Subscribing events with filter for properties:" << std::endl;
    for ( Node& child: eventtype.GetProperties() )
    {
      if (Debug) std::cout << "      property: "<< child.GetName() << std::endl;
      SimpleAttributeOperand op;
      op.TypeID = eventtype.GetId();
      op.Attribute = AttributeID::Value;
      op.BrowsePath = std::vector<QualifiedName>({child.GetName()});
      filter.SelectClauses.push_back(op);
    }
    return SubscribeEvents(node, filter);
  }

  uint32_t Subscription::SubscribeEvents(const Node& node, const EventFilter& eventfilter)
  {
    std::unique_lock<std::mutex> lock(Mutex); 

    MonitoredItemsParameters itemsParams;
    itemsParams.SubscriptionID = Data.ID;

    AttributeValueID avid;
    avid.Node = node.GetId();
    avid.Attribute = AttributeID::EventNotifier;

    MonitoredItemRequest req;
    req.ItemToMonitor = avid;
    req.Mode = MonitoringMode::Reporting;
    MonitoringParameters params;
    params.SamplingInterval = Data.RevisedPublishingInterval;
    params.QueueSize = std::numeric_limits<uint32_t>::max();
    params.DiscardOldest = true;
    params.ClientHandle = IntegerID(++LastMonitoredItemHandle);

    MonitoringFilter filter(eventfilter);
    params.Filter = filter;
    req.Parameters = params;
    itemsParams.ItemsToCreate.push_back(req);

    std::vector<CreateMonitoredItemsResult> results =  Server->Subscriptions()->CreateMonitoredItems(itemsParams).Results;
    if ( results.size()  != 1 )
    {
      throw(std::runtime_error("Subscription | Protocol Error CreateMonitoredItems should return one result"));
    }

    MonitoredItemData mdata;
    mdata.TargetNode = Node(Server, avid.Node);
    mdata.Attribute = avid.Attribute;
    mdata.MonitoredItemID = results[0].MonitoredItemID;
    mdata.Filter = results[0].Filter;
    AttributeValueMap[params.ClientHandle] = mdata;


    CreateMonitoredItemsResult res = results[0];
    CheckStatusCode(res.Status);
    SimpleAttributeOperandMap[res.MonitoredItemID] = eventfilter; //Not used
    return res.MonitoredItemID;
  }

}
