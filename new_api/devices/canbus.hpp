#pragma once

#include <blmc_can/blmc_can.h>

#include <memory>
#include <string>

#include <utils/timer.hpp>
#include <utils/threadsafe_object.hpp>
#include <utils/threadsafe_timeseries.hpp>

#include <utils/os_interface.hpp>



class CanFrame
{
public:
    std::array<uint8_t, 8> data;
    uint8_t dlc;
    can_id_t id;
};

class CanConnection
{
public:
    struct sockaddr_can send_addr;
    int socket;
};

class CanbusInterface
{
public:
    typedef ThreadsafeTimeseriesInterface<CanFrame> CanframeTimeseries;

    /// output =================================================================
    virtual std::shared_ptr<const CanframeTimeseries> output() const = 0;

    /// input ==================================================================
    virtual std::shared_ptr<CanframeTimeseries> input() = 0;
    virtual void send_if_input_changed() = 0;

    /// ========================================================================

    virtual ~CanbusInterface() {}
};

class XenomaiCanbus: public CanbusInterface
{
public:

    /// output =================================================================
    std::shared_ptr<const CanframeTimeseries> output() const
    {
        return output_;
    }

    /// input ==================================================================
    virtual std::shared_ptr<CanframeTimeseries>  input()
    {
        return input_;
    }
    virtual void send_if_input_changed()
    {
        long int new_hash = input_->next_timeindex();
        if(new_hash != input_hash_.get())
        {
            send_frame(input_->current_element());
            input_hash_.set(new_hash);
        }
    }

    /// ========================================================================
    XenomaiCanbus(std::string can_interface_name)
    {
        input_ = std::make_shared<ThreadsafeTimeseries<CanFrame>>(1000);
        output_ = std::make_shared<ThreadsafeTimeseries<CanFrame>>(1000);
        input_hash_.set(input_->next_timeindex());


        // setup can connection --------------------------------
        // \todo get rid of old format stuff
//        CAN_CanConnection_t can_connection_old_format;
//        int ret = setup_can(&can_connection_old_format,
//                            can_interface_name.c_str(), 0);
//        if (ret < 0)
//        {
//            osi::print_to_screen("Couldn't setup CAN connection. Exit.");
//            exit(-1);
//        }

        CanConnection can_connection = setup_can(can_interface_name, 0);
        connection_info_.set(can_connection);


        osi::start_thread(&XenomaiCanbus::loop, this);
    }

    virtual ~XenomaiCanbus()
    {
        osi::close_can_device(connection_info_.get().socket);
    }

    /// private attributes and methods =========================================
private:
    // attributes --------------------------------------------------------------
    SingletypeThreadsafeObject<CanConnection, 1> connection_info_;

    std::shared_ptr<ThreadsafeTimeseriesInterface<CanFrame>> input_;
    SingletypeThreadsafeObject<long int, 1> input_hash_;
    std::shared_ptr<ThreadsafeTimeseriesInterface<CanFrame>> output_;

    // methods -----------------------------------------------------------------
    static void
#ifndef __XENO__
    *
#endif
    loop(void* instance_pointer)
    {
        ((XenomaiCanbus*)(instance_pointer))->loop();
    }

    void loop()
    {
        Timer<100> loop_time_logger("can bus loop", 4000);
        Timer<100> receive_time_logger("receive", 4000);

        while (true)
        {
            receive_time_logger.start_interval();
            CanFrame frame = receive_frame();
            receive_time_logger.end_interval();
            output_->append(frame);
            loop_time_logger.end_and_start_interval();
        }
    }

    // send input data ---------------------------------------------------------
    void send_frame(const CanFrame& unstamped_can_frame)
    {
        // get address ---------------------------------------------------------
        int socket = connection_info_.get().socket;
        struct sockaddr_can address = connection_info_.get().send_addr;

        // put data into can frame ---------------------------------------------
        can_frame_t can_frame;
        can_frame.can_id = unstamped_can_frame.id;
        can_frame.can_dlc = unstamped_can_frame.dlc;

        memcpy(can_frame.data, unstamped_can_frame.data.begin(),
               unstamped_can_frame.dlc);

        // send ----------------------------------------------------------------
        osi::send_to_can_device(socket,
                                (void *)&can_frame,
                                sizeof(can_frame_t),
                                0,
                                (struct sockaddr *)&address,
                                sizeof(address));
    }


    CanFrame receive_frame()
    {
        int socket = connection_info_.get().socket;

        // data we want to obtain ----------------------------------------------
        can_frame_t can_frame;
        nanosecs_abs_t timestamp;
        struct sockaddr_can message_address;

        // setup message such that data can be received to variables above -----
        struct iovec input_output_vector;
        input_output_vector.iov_base = (void *)&can_frame;
        input_output_vector.iov_len = sizeof(can_frame_t);

        struct msghdr message_header;
        message_header.msg_iov = &input_output_vector;
        message_header.msg_iovlen = 1;
        message_header.msg_name = (void *)&message_address;
        message_header.msg_namelen = sizeof(struct sockaddr_can);
        message_header.msg_control = (void *)&timestamp;
        message_header.msg_controllen = sizeof(nanosecs_abs_t);

        // receive message from can bus ----------------------------------------
        osi::receive_message_from_can_device(socket, &message_header, 0);

        // process received data and put into felix widmaier's format ----------
        if (message_header.msg_controllen == 0)
        {
            // No timestamp for this frame available. Make sure we dont get
            // garbage.
            timestamp = 0;
        }

        CanFrame out_frame;
        out_frame.id = can_frame.can_id;
        out_frame.dlc = can_frame.can_dlc;
        for(size_t i = 0; i < can_frame.can_dlc; i++)
        {
            out_frame.data[i] = can_frame.data[i];
        }

        return out_frame;
    }

    CanConnection setup_can(std::string name, uint32_t err_mask)
    {
        int socket;
        sockaddr_can recv_addr;
        sockaddr_can send_addr;


        CAN_CanConnection_t temp;
        CAN_CanConnection_t *can = &temp;
        int ret;
        struct ifreq ifr;


        ret = rt_dev_socket(PF_CAN, SOCK_RAW, CAN_RAW);
        if (ret < 0) {
            rt_fprintf(stderr, "rt_dev_socket: %s\n", strerror(-ret));
            osi::print_to_screen("Couldn't setup CAN connection. Exit.");
            exit(-1);
        }
        socket = ret;


        strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ);
        ret = rt_dev_ioctl(socket, SIOCGIFINDEX, &ifr);
        if (ret < 0)
        {
            rt_fprintf(stderr, "rt_dev_ioctl GET_IFINDEX: %s\n",
                       strerror(-ret));
            osi::close_can_device(socket);
            osi::print_to_screen("Couldn't setup CAN connection. Exit.");
            exit(-1);
        }



        // Set error mask
        if (err_mask) {
            ret = rt_dev_setsockopt(socket, SOL_CAN_RAW, CAN_RAW_ERR_FILTER,
                                    &err_mask, sizeof(err_mask));
            if (ret < 0)
            {
                rt_fprintf(stderr, "rt_dev_setsockopt: %s\n", strerror(-ret));
                osi::close_can_device(socket);
                osi::print_to_screen("Couldn't setup CAN connection. Exit.");
                exit(-1);
            }
        }

        //if (filter_count) {
        //    ret = rt_dev_setsockopt(s, SOL_CAN_RAW, CAN_RAW_FILTER,
        //                            &recv_filter, filter_count *
        //                            sizeof(struct can_filter));
        //    if (ret < 0) {
        //        rt_fprintf(stderr, "rt_dev_setsockopt: %s\n", strerror(-ret));
        //        goto failure;
        //    }
        //}

        // Bind to socket
        recv_addr.can_family = AF_CAN;
        recv_addr.can_ifindex = ifr.ifr_ifindex;
        ret = rt_dev_bind(socket, (struct sockaddr *)&recv_addr,
                          sizeof(struct sockaddr_can));
        if (ret < 0)
        {
            rt_fprintf(stderr, "rt_dev_bind: %s\n", strerror(-ret));
            osi::close_can_device(socket);
            osi::print_to_screen("Couldn't setup CAN connection. Exit.");
            exit(-1);
        }

#ifdef __XENO__
        // Enable timestamps for frames
        ret = rt_dev_ioctl(socket,
                           RTCAN_RTIOC_TAKE_TIMESTAMP, RTCAN_TAKE_TIMESTAMPS);
        if (ret) {
            rt_fprintf(stderr, "rt_dev_ioctl TAKE_TIMESTAMP: %s\n",
                       strerror(-ret));
            osi::close_can_device(socket);
            osi::print_to_screen("Couldn't setup CAN connection. Exit.");
            exit(-1);
        }
#elif defined __RT_PREEMPT__
        // TODO: Need to support timestamps.
#endif

        recv_addr.can_family = AF_CAN;
        recv_addr.can_ifindex = ifr.ifr_ifindex;

        can->msg.msg_iov = &can->iov;
        can->msg.msg_iovlen = 1;
        can->msg.msg_name = (void *)&can->msg_addr;
        can->msg.msg_namelen = sizeof(struct sockaddr_can);
        can->msg.msg_control = (void *)&can->timestamp;
        can->msg.msg_controllen = sizeof(nanosecs_abs_t);

        // TODO why the memset?
        memset(&send_addr, 0, sizeof(send_addr));
        send_addr.can_family = AF_CAN;
        send_addr.can_ifindex = ifr.ifr_ifindex;

        CanConnection can_connection;
        can_connection.send_addr = send_addr;
        can_connection.socket = socket;

        return can_connection;
    }
};