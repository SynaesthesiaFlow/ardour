/*
    Copyright (C) 2002 Paul Davis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#ifndef __ardour_transport_master_h__
#define __ardour_transport_master_h__

#include <vector>

#include <boost/atomic.hpp>
#include <boost/optional.hpp>

#include <glibmm/threads.h>

#include <ltc.h>

#include "pbd/signals.h"

#include "temporal/time.h"

#include "ardour/libardour_visibility.h"
#include "ardour/types.h"
#include "midi++/parser.h"
#include "midi++/types.h"


/* used for delta_string(): */
#define PLUSMINUS(A) ( ((A)<0) ? "-" : (((A)>0) ? "+" : "\u00B1") )
#define LEADINGZERO(A) ( (A)<10 ? "   " : (A)<100 ? "  " : (A)<1000 ? " " : "" )

namespace ARDOUR {

class TempoMap;
class Session;
class AudioEngine;
class Location;
class MidiPort;
class AudioPort;
class Port;


/**
 * @class TransportMaster
 *
 * @brief The TransportMaster interface can be used to sync ARDOURs tempo to an external source
 * like MTC, MIDI Clock, etc. as well as a single internal pseudo master we
 * call "UI" because it is controlled from any of the user interfaces for
 * Ardour (GUI, control surfaces, OSC, etc.)
 *
 */
class LIBARDOUR_API TransportMaster {
  public:

	TransportMaster (SyncSource t, std::string const & name);
	virtual ~TransportMaster();

	static boost::shared_ptr<TransportMaster> factory (SyncSource, std::string const &);
	static boost::shared_ptr<TransportMaster> factory (XMLNode const &);

	virtual void pre_process (pframes_t nframes, samplepos_t now, boost::optional<samplepos_t>) = 0;

	/**
	 * This is the most important function to implement:
	 * Each process cycle, Session::follow_slave will call this method.
	 *  and after the method call they should
	 *
	 * Session::follow_slave will then try to follow the given
	 * <em>position</em> using a delay locked loop (DLL),
	 * starting with the first given transport speed.
	 * If the values of speed and position contradict each other,
	 * ARDOUR will always follow the position and disregard the speed.
	 * Although, a correct speed is important so that ARDOUR
	 * can sync to the master time source quickly.
	 *
	 * For background information on delay locked loops,
	 * see http://www.kokkinizita.net/papers/usingdll.pdf
	 *
	 * The method has the following precondition:
	 * <ul>
	 *   <li>
	 *       TransportMaster::ok() should return true, otherwise playback will stop
	 *       immediately and the method will not be called
	 *   </li>
	 *   <li>
	 *     when the references speed and position are passed into the TransportMaster
	 *     they are uninitialized
	 *   </li>
	 * </ul>
	 *
	 * After the method call the following postconditions should be met:
	 * <ul>
	 *    <li>
	 *       The first position value on transport start should be 0,
	 *       otherwise ARDOUR will try to locate to the new position
	 *       rather than move to it
	 *    </li>
	 *    <li>
	 *      the references speed and position should be assigned
	 *      to the TransportMasters current requested transport speed
	 *      and transport position.
	 *    </li>
	 *   <li>
	 *     TransportMaster::resolution() should be greater than the maximum distance of
	 *     ARDOURs transport position to the slaves requested transport position.
	 *   </li>
	 *   <li>TransportMaster::locked() should return true, otherwise Session::no_roll will be called</li>
	 *   <li>TransportMaster::starting() should be false, otherwise the transport will not move until it becomes true</li>	 *
	 * </ul>
	 *
	 * @param speed - The transport speed requested
	 * @param position - The transport position requested
	 * @return - The return value is currently ignored (see Session::follow_slave)
	 */
	virtual bool speed_and_position (double& speed, samplepos_t& position, samplepos_t now) = 0;

	/**
	 * reports to ARDOUR whether the TransportMaster is currently synced to its external
	 * time source.
	 *
	 * @return - when returning false, the transport will stop rolling
	 */
	virtual bool locked() const = 0;

	/**
	 * reports to ARDOUR whether the slave is in a sane state
	 *
	 * @return - when returning false, the transport will be stopped and the slave
	 * disconnected from ARDOUR.
	 */
	virtual bool ok() const = 0;

	/**
	 * reports to ARDOUR whether the slave is in the process of starting
	 * to roll
	 *
	 * @return - when returning false, transport will not move until this method returns true
	 */
	virtual bool starting() const { return false; }

	/**
	 * @return - the timing resolution of the TransportMaster - If the distance of ARDOURs transport
	 * to the slave becomes greater than the resolution, sound will stop
	 */
	virtual samplecnt_t resolution() const = 0;

	/**
	 * @return - when returning true, ARDOUR will wait for seekahead_distance() before transport
	 * starts rolling
	 */
	virtual bool requires_seekahead () const = 0;

	/**
	 * @return the number of samples that this slave wants to seek ahead. Relevant
	 * only if requires_seekahead() returns true.
	 */

	virtual samplecnt_t seekahead_distance() const { return 0; }

	/**
	 * @return - when returning true, ARDOUR will use transport speed 1.0 no matter what
	 *           the slave returns
	 */
	virtual bool sample_clock_synced() const { return false; }

	/**
	 * @return - current time-delta between engine and sync-source
	 */
	virtual std::string delta_string() const { return ""; }

	sampleoffset_t current_delta() const { return _current_delta; }

	/* this is intended to be used by a UI and polled from a timeout. it should
	   return a string describing the current position of the TC source. it
	   should NOT do any computation, but should use a cached value
	   of the TC source position.
	*/
	virtual std::string position_string() const = 0;

	virtual bool can_loop() const { return false; }

	virtual Location* loop_location() const { return 0; }
	bool has_loop() const { return loop_location() != 0; }

	SyncSource type() const { return _type; }
	TransportRequestSource request_type() const {
		switch (_type) {
		case Engine: /* also JACK */
			return TRS_Engine;
		case MTC:
			return TRS_MTC;
		case LTC:
			return TRS_LTC;
		case MIDIClock:
			break;
		}
		return TRS_MIDIClock;
	}

	std::string name() const { return _name; }
	void set_name (std::string const &);

	int set_state (XMLNode const &, int);
	XMLNode& get_state();

	static const std::string state_node_name;

	virtual void set_session (Session*);

	boost::shared_ptr<Port> port() const { return _port; }

	bool check_collect();
	virtual void set_collect (bool);

	/* called whenever the manager starts collecting (processing) this
	   transport master. Typically will re-initialize any state used to
	   deal with incoming data.
	*/
	virtual void init() = 0;

  protected:
	SyncSource      _type;
	std::string     _name;
	Session*        _session;
	bool            _connected;
	sampleoffset_t  _current_delta;
	bool            _collect;
	bool            _pending_collect;

	/* DLL - chase incoming data */

	int    transport_direction;
	int    dll_initstate;

	double t0;
	double t1;
	double e2;
	double b, c;

	boost::shared_ptr<Port>  _port;

	PBD::ScopedConnection port_connection;
	bool connection_handler (boost::weak_ptr<ARDOUR::Port>, std::string name1, boost::weak_ptr<ARDOUR::Port>, std::string name2, bool yn);
};

struct LIBARDOUR_API SafeTime {
	volatile int guard1;
	samplepos_t   position;
	samplepos_t   timestamp;
	double       speed;
	volatile int guard2;

	SafeTime() {
		guard1 = 0;
		position = 0;
		timestamp = 0;
		speed = 0;
		guard2 = 0;
	}
};

/** a helper class for any TransportMaster that receives its input via a MIDI
 * port
 */
class LIBARDOUR_API TransportMasterViaMIDI {
  public:
	boost::shared_ptr<MidiPort> midi_port() const { return _midi_port; }
	boost::shared_ptr<Port> create_midi_port (std::string const & port_name);

  protected:
	TransportMasterViaMIDI () {};

	MIDI::Parser                 parser;
	boost::shared_ptr<MidiPort> _midi_port;

	void update_from_midi (pframes_t nframes, samplepos_t now);
};

class LIBARDOUR_API TimecodeTransportMaster : public TransportMaster {
  public:
	TimecodeTransportMaster (std::string const & name, SyncSource type) : TransportMaster (type, name) {}

	virtual Timecode::TimecodeFormat apparent_timecode_format() const = 0;

	samplepos_t        timecode_offset;
	bool              timecode_negative_offset;
};

class LIBARDOUR_API MTC_TransportMaster : public TimecodeTransportMaster, public TransportMasterViaMIDI {
  public:
	MTC_TransportMaster (std::string const &);
	~MTC_TransportMaster ();

	void set_session (Session*);

	void pre_process (pframes_t nframes, samplepos_t now, boost::optional<samplepos_t>);

	bool speed_and_position (double&, samplepos_t&, samplepos_t);

	bool locked() const;
	bool ok() const;
	void handle_locate (const MIDI::byte*);

	samplecnt_t resolution () const;
	bool requires_seekahead () const { return false; }
	samplecnt_t seekahead_distance() const;
	void init ();

        Timecode::TimecodeFormat apparent_timecode_format() const;
        std::string position_string() const;
	std::string delta_string() const;

  private:
	PBD::ScopedConnectionList port_connections;
	PBD::ScopedConnection     config_connection;
	bool        can_notify_on_unknown_rate;

	static const int sample_tolerance;

	SafeTime       current;
	samplepos_t    mtc_frame;               /* current time */
	double         mtc_frame_dll;
	samplepos_t    last_inbound_frame;      /* when we got it; audio clocked */
	MIDI::byte     last_mtc_fps_byte;
	samplepos_t    window_begin;
	samplepos_t    window_end;
	samplepos_t    first_mtc_timestamp;
	bool           did_reset_tc_format;
	Timecode::TimecodeFormat saved_tc_format;
	Glib::Threads::Mutex    reset_lock;
	uint32_t       reset_pending;
	bool           reset_position;
	int            transport_direction;
	int            busy_guard1;
	int            busy_guard2;

	double         speedup_due_to_tc_mismatch;
	double         quarter_frame_duration;
	Timecode::TimecodeFormat mtc_timecode;
	Timecode::TimecodeFormat a3e_timecode;
	Timecode::Time timecode;
	bool           printed_timecode_warning;

	void reset (bool with_pos);
	void queue_reset (bool with_pos);
	void maybe_reset ();

	void update_mtc_qtr (MIDI::Parser&, int, samplepos_t);
	void update_mtc_time (const MIDI::byte *, bool, samplepos_t);
	void update_mtc_status (MIDI::MTC_Status);
	void read_current (SafeTime *) const;
	void reset_window (samplepos_t);
	bool outside_window (samplepos_t) const;
	void init_mtc_dll(samplepos_t, double);
	void parse_timecode_offset();
	void parameter_changed(std::string const & p);
};

class LIBARDOUR_API LTC_TransportMaster : public TimecodeTransportMaster {
public:
	LTC_TransportMaster (std::string const &);
	~LTC_TransportMaster ();

	void set_session (Session*);

	void pre_process (pframes_t nframes, samplepos_t now, boost::optional<samplepos_t>);
	bool speed_and_position (double&, samplepos_t&, samplepos_t);

	bool locked() const;
	bool ok() const;

	samplecnt_t resolution () const;
	bool requires_seekahead () const { return false; }
	samplecnt_t seekahead_distance () const { return 0; }
	void init ();

	Timecode::TimecodeFormat apparent_timecode_format() const;
	std::string position_string() const;
	std::string delta_string() const;

  private:
	void parse_ltc(const pframes_t, const Sample* const, const samplecnt_t);
	void process_ltc(samplepos_t const);
	void init_dll (samplepos_t, int32_t);
	bool detect_discontinuity(LTCFrameExt *, int, bool);
	bool detect_ltc_fps(int, bool);
	bool equal_ltc_sample_time(LTCFrame *a, LTCFrame *b);
	void reset (bool with_ts = true);
	void resync_xrun();
	void resync_latency();
	void parse_timecode_offset();
	void parameter_changed(std::string const & p);

	bool           did_reset_tc_format;
	Timecode::TimecodeFormat saved_tc_format;

	LTCDecoder *   decoder;
	double         samples_per_ltc_frame;
	Timecode::Time timecode;
	LTCFrameExt    prev_sample;
	bool           fps_detected;

	samplecnt_t     monotonic_cnt;
	samplecnt_t     last_timestamp;
	samplecnt_t     last_ltc_sample;
	double          ltc_speed;
	int            delayedlocked;

	int            ltc_detect_fps_cnt;
	int            ltc_detect_fps_max;
	bool           printed_timecode_warning;
	bool           sync_lock_broken;
	Timecode::TimecodeFormat ltc_timecode;
	Timecode::TimecodeFormat a3e_timecode;
	double         samples_per_timecode_frame;

	PBD::ScopedConnectionList port_connections;
	PBD::ScopedConnection     config_connection;
        LatencyRange  ltc_slave_latency;
};

class LIBARDOUR_API MIDIClock_TransportMaster : public TransportMaster, public TransportMasterViaMIDI {
  public:
	MIDIClock_TransportMaster (std::string const & name, int ppqn = 24);

	/// Constructor for unit tests
	~MIDIClock_TransportMaster ();

	void set_session (Session*);

	void pre_process (pframes_t nframes, samplepos_t now, boost::optional<samplepos_t>);

	void rebind (MidiPort&);
	bool speed_and_position (double&, samplepos_t&, samplepos_t);

	bool locked() const;
	bool ok() const;
	bool starting() const;

	samplecnt_t resolution () const;
	bool requires_seekahead () const { return false; }
	void init ();

	std::string position_string() const;
	std::string delta_string() const;

  protected:
	PBD::ScopedConnectionList port_connections;

	/// pulses per quarter note for one MIDI clock sample (default 24)
	int         ppqn;

	/// the duration of one ppqn in sample time
	double      one_ppqn_in_samples;

	/// the timestamp of the first MIDI clock message
	samplepos_t  first_timestamp;

	/// the time stamp and should-be transport position of the last inbound MIDI clock message
	samplepos_t  last_timestamp;
	double      should_be_position;

	/// the number of midi clock messages received (zero-based)
	/// since start
	long midi_clock_count;

	/// a DLL to track MIDI clock

	double _speed;
	bool _running;
	double _bpm;

	void reset ();
	void start (MIDI::Parser& parser, samplepos_t timestamp);
	void contineu (MIDI::Parser& parser, samplepos_t timestamp);
	void stop (MIDI::Parser& parser, samplepos_t timestamp);
	void position (MIDI::Parser& parser, MIDI::byte* message, size_t size);
	// we can't use continue because it is a C++ keyword
	void calculate_one_ppqn_in_samples_at(samplepos_t time);
	samplepos_t calculate_song_position(uint16_t song_position_in_sixteenth_notes);
	void calculate_filter_coefficients (double qpm);
	void update_midi_clock (MIDI::Parser& parser, samplepos_t timestamp);
	void read_current (SafeTime *) const;

	static const int accumulator_capacity = 10;
	double accumulator[accumulator_capacity];
	int accumulator_index;
	int accumulator_size;

	void accumulator_reset ();
	double accumulator_average();
};

class LIBARDOUR_API Engine_TransportMaster : public TransportMaster
{
  public:
	Engine_TransportMaster (AudioEngine&);
	~Engine_TransportMaster  ();

	void pre_process (pframes_t nframes, samplepos_t now,  boost::optional<samplepos_t>);
	bool speed_and_position (double& speed, samplepos_t& pos, samplepos_t);

	bool starting() const { return _starting; }
	bool locked() const;
	bool ok() const;
	samplecnt_t resolution () const { return 1; }
	bool requires_seekahead () const { return false; }
	bool sample_clock_synced() const { return true; }
	void init ();

	std::string position_string() const;
	std::string delta_string() const;

  private:
        AudioEngine& engine;
        bool _starting;
};

} /* namespace */

#endif /* __ardour_transport_master_h__ */