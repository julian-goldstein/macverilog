/*
 * Copyright (c) 1998-2006 Stephen Williams (steve@icarus.com)
 *
 *    This source code is free software; you can redistribute it
 *    and/or modify it in source code form under the terms of the GNU
 *    General Public License as published by the Free Software
 *    Foundation; either version 2 of the License, or (at your option)
 *    any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */
#ifdef HAVE_CVS_IDENT
#ident "$Id: elaborate.cc,v 1.353 2006/12/08 04:09:41 steve Exp $"
#endif

# include "config.h"

/*
 * Elaboration takes as input a complete parse tree and the name of a
 * root module, and generates as output the elaborated design. This
 * elaborated design is presented as a Module, which does not
 * reference any other modules. It is entirely self contained.
 */

# include  <typeinfo>
# include  <sstream>
# include  <list>
# include  "pform.h"
# include  "PEvent.h"
# include  "PGenerate.h"
# include  "PSpec.h"
# include  "netlist.h"
# include  "netmisc.h"
# include  "util.h"
# include  "parse_api.h"
# include  "compiler.h"


static Link::strength_t drive_type(PGate::strength_t drv)
{
      switch (drv) {
	  case PGate::HIGHZ:
	    return Link::HIGHZ;
	  case PGate::WEAK:
	    return Link::WEAK;
	  case PGate::PULL:
	    return Link::PULL;
	  case PGate::STRONG:
	    return Link::STRONG;
	  case PGate::SUPPLY:
	    return Link::SUPPLY;
	  default:
	    assert(0);
      }
      return Link::STRONG;
}


void PGate::elaborate(Design*des, NetScope*scope) const
{
      cerr << "internal error: what kind of gate? " <<
	    typeid(*this).name() << endl;
}

/*
 * Elaborate the continuous assign. (This is *not* the procedural
 * assign.) Elaborate the lvalue and rvalue, and do the assignment.
 */
void PGAssign::elaborate(Design*des, NetScope*scope) const
{
      assert(scope);

      NetExpr* rise_time, *fall_time, *decay_time;
      eval_delays(des, scope, rise_time, fall_time, decay_time, true);

      Link::strength_t drive0 = drive_type(strength0());
      Link::strength_t drive1 = drive_type(strength1());

      assert(pin(0));
      assert(pin(1));

	/* Elaborate the l-value. */
      NetNet*lval = pin(0)->elaborate_lnet(des, scope);
      if (lval == 0) {
	    des->errors += 1;
	    return;
      }

      assert(lval->pin_count() == 1);

      if (debug_elaborate) {
	    cerr << get_line() << ": debug: PGassign: elaborated l-value"
		 << " width=" << lval->vector_width()
		 << ", type=" << lval->data_type() << endl;
      }

	/* Handle the special case that the rval is simply an
	   identifier. Get the rval as a NetNet, then use NetBUFZ
	   objects to connect it to the l-value. This is necessary to
	   direct drivers. This is how I attach strengths to the
	   assignment operation. */
      if (const PEIdent*id = dynamic_cast<const PEIdent*>(pin(1))) {
	    NetNet*rid = id->elaborate_net(des, scope, lval->vector_width(),
					   0, 0, 0, Link::STRONG,
					   Link::STRONG);
	    if (rid == 0) {
		  des->errors += 1;
		  return;
	    }

	    assert(rid);
	    assert(rid->pin_count() == 1);

	      /* If the right hand net is the same type as the left
		 side net (i.e., WIRE/WIRE) then it is enough to just
		 connect them together. Otherwise, put a bufz between
		 them to carry strengths from the rval.

		 While we are at it, handle the case where the r-value
		 is not as wide as the l-value by padding with a
		 constant-0. */

	    unsigned cnt = lval->vector_width();
	    if (rid->vector_width() < cnt)
		  cnt = rid->vector_width();

	    bool need_driver_flag = false;

	      /* If the device is linked to itself, a driver is
		 needed. Should I print a warning here? */
	    if (lval->pin(0) .is_linked (rid->pin(0)))
		  need_driver_flag = true;

	      /* If the nets are different type (i.e., reg vs. tri) then
		 a driver is needed. */
	    if (rid->type() != lval->type())
		  need_driver_flag = true;

	      /* If there is a delay, then I need a driver to carry
		 it. */
	    if (rise_time || fall_time || decay_time)
		  need_driver_flag = true;

	      /* If there is a strength to be carried, then I need a
		 driver to carry that strength. */
	    if (rid->pin(0).drive0() != drive0)
		  need_driver_flag = true;

	    if (rid->pin(0).drive1() != drive1)
		  need_driver_flag = true;

	      /* If the r-value is more narrow then the l-value, pad
		 it to the desired width. */
	    if (cnt < lval->vector_width()) {
		  if (lval->get_signed() && rid->get_signed()) {

			unsigned use_width = lval->vector_width();

			if (debug_elaborate)
			      cerr << get_line() << ": debug: PGassign "
				   << "Generate sign-extend node." << endl;

			rid = pad_to_width_signed(des, rid, use_width);

		  } else {

			if (debug_elaborate)
			      cerr << get_line() << ": debug: PGAssign "
				   << "Unsigned pad r-value from "
				   << cnt << " bits to "
				   << lval->vector_width() << " bits." << endl;

			NetNet*tmp = pad_to_width(des, rid,
						  lval->vector_width());
			rid = tmp;
		  }

	    } else if (cnt < rid->vector_width()) {

		  if (debug_elaborate)
			cerr << get_line() << ": debug: PGAssign "
			     << "Truncate r-value from "
			     << cnt << " bits to "
			     << lval->vector_width() << " bits." << endl;

		  NetNet*tmp = crop_to_width(des, rid, lval->vector_width());
		  rid = tmp;
	    }

	    if (! need_driver_flag) {

		    /* Don't need a driver, presumably because the
		       r-value already has the needed drivers. Just
		       hook things up. If the r-value is too narrow
		       for the l-value, then sign extend it or zero
		       extend it, whichever makes sense. */

		  if (debug_elaborate) {
			cerr << get_line() << ": debug: PGAssign: "
			     << "Connect lval directly to "
			     << id->path() << endl;
		  }

		  connect(lval->pin(0), rid->pin(0));

	    } else {
		    /* Do need a driver. Use BUFZ objects to carry the
		       strength and delays. */

		  if (debug_elaborate) {
			cerr << get_line() << ": debug: PGAssign: "
			     << "Connect lval to " << id->path()
			     << " through bufz. delay=(";
			if (rise_time)
			      cerr << *rise_time << ":";
			else
			      cerr << "<none>:";
			if (fall_time)
			      cerr << *fall_time << ":";
			else
			      cerr << "<none>:";
			if (decay_time)
			      cerr << *decay_time;
			else
			      cerr << "<none>";
			cerr << ")" << endl;
		  }

		  NetBUFZ*dev = new NetBUFZ(scope, scope->local_symbol(),
					    rid->vector_width());
		  connect(lval->pin(0), dev->pin(0));
		  connect(rid->pin(0),  dev->pin(1));
		  dev->rise_time(rise_time);
		  dev->fall_time(fall_time);
		  dev->decay_time(decay_time);
		  dev->pin(0).drive0(drive0);
		  dev->pin(0).drive1(drive1);
		  des->add_node(dev);

	    }

	    return;
      }

	/* Elaborate the r-value. Account for the initial decays,
	   which are going to be attached to the last gate before the
	   generated NetNet. */
      NetNet*rval = pin(1)->elaborate_net(des, scope,
					  lval->vector_width(),
					  rise_time, fall_time, decay_time,
					  drive0, drive1);
      if (rval == 0) {
	    cerr << get_line() << ": error: Unable to elaborate r-value: "
		 << *pin(1) << endl;
	    des->errors += 1;
	    return;
      }

      if (debug_elaborate) {
	    cerr << get_line() << ": debug: PGAssign: elaborated r-value"
		 << " width="<<rval->vector_width()
		 << ", type="<< rval->data_type() << endl;
      }

      assert(lval && rval);
      assert(rval->pin_count() == 1);

	/* If the r-value insists on being smaller then the l-value
	   (perhaps it is explicitly sized) the pad it out to be the
	   right width so that something is connected to all the bits
	   of the l-value. */
      if (lval->vector_width() > rval->vector_width())
	    rval = pad_to_width(des, rval, lval->vector_width());

	/* If, on the other hand, the r-value insists on being
	   LARGER then the l-value, use a part select to chop it down
	   down to size. */
      if (lval->vector_width() < rval->vector_width()) {
	    NetPartSelect*tmp = new NetPartSelect(rval, 0,lval->vector_width(),
						  NetPartSelect::VP);
	    des->add_node(tmp);
	    tmp->set_line(*this);
	    NetNet*osig = new NetNet(scope, scope->local_symbol(),
				     NetNet::TRI, lval->vector_width());
	    osig->set_line(*this);
	    osig->data_type(rval->data_type());
	    connect(osig->pin(0), tmp->pin(0));
	    rval = osig;
      }

      connect(lval->pin(0), rval->pin(0));

      if (lval->local_flag())
	    delete lval;

}

/*
 * Elaborate a Builtin gate. These normally get translated into
 * NetLogic nodes that reflect the particular logic function.
 */
void PGBuiltin::elaborate(Design*des, NetScope*scope) const
{
      unsigned count = 1;
      unsigned instance_width = 1;
      long low = 0, high = 0;
      string name = string(get_name());

      if (name == "")
	    name = scope->local_symbol();

	/* If the Verilog source has a range specification for the
	   gates, then I am expected to make more then one
	   gate. Figure out how many are desired. */
      if (msb_) {
	    NetExpr*msb_exp = elab_and_eval(des, scope, msb_, -1);
	    NetExpr*lsb_exp = elab_and_eval(des, scope, lsb_, -1);

	    NetEConst*msb_con = dynamic_cast<NetEConst*>(msb_exp);
	    NetEConst*lsb_con = dynamic_cast<NetEConst*>(lsb_exp);

	    if (msb_con == 0) {
		  cerr << get_line() << ": error: Unable to evaluate "
			"expression " << *msb_ << endl;
		  des->errors += 1;
		  return;
	    }

	    if (lsb_con == 0) {
		  cerr << get_line() << ": error: Unable to evaluate "
			"expression " << *lsb_ << endl;
		  des->errors += 1;
		  return;
	    }

	    verinum msb = msb_con->value();
	    verinum lsb = lsb_con->value();

	    delete msb_exp;
	    delete lsb_exp;

	    if (msb.as_long() > lsb.as_long())
		  count = msb.as_long() - lsb.as_long() + 1;
	    else
		  count = lsb.as_long() - msb.as_long() + 1;

	    low = lsb.as_long();
	    high = msb.as_long();

	    if (debug_elaborate) {
		  cerr << get_line() << ": debug: PGBuiltin: Make arrray "
		       << "[" << high << ":" << low << "]"
		       << " of " << count << " gates for " << name << endl;
	    }
      }

	/* Now we have a gate count. Elaborate the output expression
	   only. We do it early so that we can see if we can make a
	   wide gate instead of an array of gates. */

      NetNet*lval_sig = pin(0)->elaborate_lnet(des, scope, true);
      assert(lval_sig);

	/* Detect the special case that the l-value width exactly
	   matches the gate count. In this case, we will make a single
	   gate that has the desired vector width. */
      if (lval_sig->vector_width() == count) {
	    instance_width = count;
	    count = 1;

	    if (debug_elaborate && instance_width != 1)
		  cerr << get_line() << ": debug: PGBuiltin: "
			"Collapsed gate array into single wide "
			"(" << instance_width << ") instance." << endl;
      }

	/* Allocate all the netlist nodes for the gates. */
      NetLogic**cur = new NetLogic*[count];
      assert(cur);

	/* Calculate the gate delays from the delay expressions
	   given in the source. For logic gates, the decay time
	   is meaningless because it can never go to high
	   impedance. However, the bufif devices can generate
	   'bz output, so we will pretend that anything can.

	   If only one delay value expression is given (i.e., #5
	   nand(foo,...)) then rise, fall and decay times are
	   all the same value. If two values are given, rise and
	   fall times are use, and the decay time is the minimum
	   of the rise and fall times. Finally, if all three
	   values are given, they are taken as specified. */

      NetExpr* rise_time, *fall_time, *decay_time;
      eval_delays(des, scope, rise_time, fall_time, decay_time);

      struct attrib_list_t*attrib_list = 0;
      unsigned attrib_list_n = 0;
      attrib_list = evaluate_attributes(attributes, attrib_list_n,
					des, scope);

	/* Now make as many gates as the bit count dictates. Give each
	   a unique name, and set the delay times. */

      for (unsigned idx = 0 ;  idx < count ;  idx += 1) {
	    ostringstream tmp;
	    unsigned index;
	    if (low < high)
		  index = low + idx;
	    else
		  index = low - idx;

	    tmp << name << "<" << index << ">";
	    perm_string inm = lex_strings.make(tmp.str());

	    switch (type()) {
		case AND:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::AND, instance_width);
		  break;
		case BUF:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::BUF, instance_width);
		  break;
		case BUFIF0:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::BUFIF0, instance_width);
		  break;
		case BUFIF1:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::BUFIF1, instance_width);
		  break;
		case NAND:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::NAND, instance_width);
		  break;
		case NMOS:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::NMOS, instance_width);
		  break;
		case NOR:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::NOR, instance_width);
		  break;
		case NOT:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::NOT, instance_width);
		  break;
		case NOTIF0:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::NOTIF0, instance_width);
		  break;
		case NOTIF1:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::NOTIF1, instance_width);
		  break;
		case OR:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::OR, instance_width);
		  break;
		case RNMOS:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::RNMOS, instance_width);
		  break;
		case RPMOS:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::RPMOS, instance_width);
		  break;
		case PMOS:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::PMOS, instance_width);
		  break;
		case PULLDOWN:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::PULLDOWN, instance_width);
		  break;
		case PULLUP:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::PULLUP, instance_width);
		  break;
		case XNOR:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::XNOR, instance_width);
		  break;
		case XOR:
		  cur[idx] = new NetLogic(scope, inm, pin_count(),
					  NetLogic::XOR, instance_width);
		  break;
		default:
		  cerr << get_line() << ": internal error: unhandled "
			"gate type." << endl;
		  des->errors += 1;
		  return;
	    }

	    for (unsigned adx = 0 ;  adx < attrib_list_n ;  adx += 1)
		  cur[idx]->attribute(attrib_list[adx].key,
				      attrib_list[adx].val);

	    cur[idx]->rise_time(rise_time);
	    cur[idx]->fall_time(fall_time);
	    cur[idx]->decay_time(decay_time);

	    cur[idx]->pin(0).drive0(drive_type(strength0()));
	    cur[idx]->pin(0).drive1(drive_type(strength1()));

	    des->add_node(cur[idx]);
      }


      delete[]attrib_list;

	/* The gates have all been allocated, this loop runs through
	   the parameters and attaches the ports of the objects. */

      for (unsigned idx = 0 ;  idx < pin_count() ;  idx += 1) {
	    const PExpr*ex = pin(idx);
	    NetNet*sig = (idx == 0)
		  ? lval_sig
		  : ex->elaborate_net(des, scope, 0, 0, 0, 0);
	    if (sig == 0)
		  continue;

	    assert(sig);

	    if (count == 1) {
		    /* Handle the case where there is one gate that
		       carries the whole vector width. */

		  if (1 == sig->vector_width() && instance_width != 1) {

			assert(sig->vector_width() == 1);
			NetReplicate*rep
			      = new NetReplicate(scope,
						 scope->local_symbol(),
						 instance_width,
						 instance_width);
			rep->set_line(*this);
			des->add_node(rep);
			connect(rep->pin(1), sig->pin(0));

			sig = new NetNet(scope, scope->local_symbol(),
					 NetNet::WIRE, instance_width);
			sig->data_type(IVL_VT_LOGIC);
			sig->local_flag(true);
			sig->set_line(*this);
			connect(rep->pin(0), sig->pin(0));

		  }

		  if (instance_width != sig->vector_width()) {

			cerr << get_line() << ": error: "
			     << "Expression width " << sig->vector_width()
			     << " does not match width " << instance_width
			     << " of logic gate array port " << idx
			     << "." << endl;
			des->errors += 1;
		  }

		  connect(cur[0]->pin(idx), sig->pin(0));

	    } else if (sig->vector_width() == 1) {
		    /* Handle the case where a single bit is connected
		       repetitively to all the instances. */
		  for (unsigned gdx = 0 ;  gdx < count ;  gdx += 1)
			connect(cur[gdx]->pin(idx), sig->pin(0));

	    } else if (sig->vector_width() == count) {

		    /* Handle the general case that each bit of the
		       value is connected to a different instance. In
		       this case, the output is handled slightly
		       different from the inputs. */
		  if (idx == 0) {
			NetConcat*cc = new NetConcat(scope,
						     scope->local_symbol(),
						     sig->vector_width(),
						     count);
			des->add_node(cc);

			  /* Connect the concat to the signal. */
			connect(cc->pin(0), sig->pin(0));

			  /* Connect the outputs of the gates to the concat. */
			for (unsigned gdx = 0 ;  gdx < count ;  gdx += 1) {
			      connect(cur[gdx]->pin(0), cc->pin(gdx+1));

			      NetNet*tmp2 = new NetNet(scope,
						       scope->local_symbol(),
						       NetNet::WIRE, 1);
			      tmp2->local_flag(true);
			      tmp2->data_type(IVL_VT_LOGIC);
			      connect(cc->pin(gdx+1), tmp2->pin(0));
			}

		  } else for (unsigned gdx = 0 ;  gdx < count ;  gdx += 1) {
			  /* Use part selects to get the bits
			     connected to the inputs of out gate. */
			NetPartSelect*tmp1 = new NetPartSelect(sig, gdx, 1,
							   NetPartSelect::VP);
			tmp1->set_line(*this);
			des->add_node(tmp1);
			connect(tmp1->pin(1), sig->pin(0));
			NetNet*tmp2 = new NetNet(scope, scope->local_symbol(),
						 NetNet::WIRE, 1);
			tmp2->local_flag(true);
			tmp2->data_type(sig->data_type());
			connect(tmp1->pin(0), tmp2->pin(0));
			connect(cur[gdx]->pin(idx), tmp1->pin(0));
		  }

	    } else {
		  cerr << get_line() << ": error: Gate count of " <<
			count << " does not match net width of " <<
			sig->vector_width() << " at pin " << idx << "."
		       << endl;
		  des->errors += 1;
	    }

      }
}

/*
 * Instantiate a module by recursively elaborating it. Set the path of
 * the recursive elaboration so that signal names get properly
 * set. Connect the ports of the instantiated module to the signals of
 * the parameters. This is done with BUFZ gates so that they look just
 * like continuous assignment connections.
 */
void PGModule::elaborate_mod_(Design*des, Module*rmod, NetScope*scope) const
{

      assert(scope);

      if (debug_elaborate) {
	    cerr << get_line() << ": debug: Instantiate module "
		 << rmod->mod_name() << " with instance name "
		 << get_name() << " in scope " << scope->name() << endl;
      }

	// This is the array of pin expressions, shuffled to match the
	// order of the declaration. If the source instantiation uses
	// bind by order, this is the same as the source list.Otherwise,
	// the source list is rearranged by name binding into this list.
      svector<PExpr*>pins (rmod->port_count());

	// If the instance has a pins_ member, then we know we are
	// binding by name. Therefore, make up a pins array that
	// reflects the positions of the named ports.
      if (pins_) {
	    unsigned nexp = rmod->port_count();

	      // Scan the bindings, matching them with port names.
	    for (unsigned idx = 0 ;  idx < npins_ ;  idx += 1) {

		    // Given a binding, look at the module port names
		    // for the position that matches the binding name.
		  unsigned pidx = rmod->find_port(pins_[idx].name);

		    // If the port name doesn't exist, the find_port
		    // method will return the port count. Detect that
		    // as an error.
		  if (pidx == nexp) {
			cerr << get_line() << ": error: port ``" <<
			      pins_[idx].name << "'' is not a port of "
			     << get_name() << "." << endl;
			des->errors += 1;
			continue;
		  }

		    // If I already bound something to this port, then
		    // the pins array will already have a pointer
		    // value where I want to place this expression.
		  if (pins[pidx]) {
			cerr << get_line() << ": error: port ``" <<
			      pins_[idx].name << "'' already bound." <<
			      endl;
			des->errors += 1;
			continue;
		  }

		    // OK, do the binding by placing the expression in
		    // the right place.
		  pins[pidx] = pins_[idx].parm;
	    }


      } else if (pin_count() == 0) {

	      /* Handle the special case that no ports are
		 connected. It is possible that this is an empty
		 connect-by-name list, so we'll allow it and assume
		 that is the case. */

	    for (unsigned idx = 0 ;  idx < rmod->port_count() ;  idx += 1)
		  pins[idx] = 0;

      } else {

	      /* Otherwise, this is a positional list of port
		 connections. In this case, the port count must be
		 right. Check that is is, the get the pin list. */

	    if (pin_count() != rmod->port_count()) {
		  cerr << get_line() << ": error: Wrong number "
			"of ports. Expecting " << rmod->port_count() <<
			", got " << pin_count() << "."
		       << endl;
		  des->errors += 1;
		  return;
	    }

	      // No named bindings, just use the positional list I
	      // already have.
	    assert(pin_count() == rmod->port_count());
	    pins = get_pins();
      }

	// Elaborate these instances of the module. The recursive
	// elaboration causes the module to generate a netlist with
	// the ports represented by NetNet objects. I will find them
	// later.

      NetScope::scope_vec_t&instance = scope->instance_arrays[get_name()];
      for (unsigned inst = 0 ;  inst < instance.count() ;  inst += 1) {
	    rmod->elaborate(des, instance[inst]);
      }



	// Now connect the ports of the newly elaborated designs to
	// the expressions that are the instantiation parameters. Scan
	// the pins, elaborate the expressions attached to them, and
	// bind them to the port of the elaborated module.

	// This can get rather complicated because the port can be
	// unconnected (meaning an empty parameter is passed) connected
	// to a concatenation, or connected to an internally
	// unconnected port.

      for (unsigned idx = 0 ;  idx < pins.count() ;  idx += 1) {

	      // Skip unconnected module ports. This happens when a
	      // null parameter is passed in.

	    if (pins[idx] == 0) {

		    // While we're here, look to see if this
		    // unconnected (from the outside) port is an
		    // input. If so, consider printing a port binding
		    // warning.
		  if (warn_portbinding) {
			svector<PEIdent*> mport = rmod->get_port(idx);
			if (mport.count() == 0)
			      continue;

			NetNet*tmp = des->find_signal(instance[0],
						      mport[0]->path());
			assert(tmp);

			if (tmp->port_type() == NetNet::PINPUT) {
			      cerr << get_line() << ": warning: "
				   << "Instantiating module "
				   << rmod->mod_name()
				   << " with dangling input port "
				   << rmod->ports[idx]->name
				   << "." << endl;
			}
		  }

		  continue;
	    }


	      // Inside the module, the port is zero or more signals
	      // that were already elaborated. List all those signals
	      // and the NetNet equivalents, for all the instances.
	    svector<PEIdent*> mport = rmod->get_port(idx);
	    svector<NetNet*>prts (mport.count() * instance.count());

	    if (debug_elaborate) {
		  cerr << get_line() << ": debug: " << get_name()
		       << ": Port " << idx << " has " << prts.count()
		       << " sub-ports." << endl;
	    }

	      // Count the internal vector bits of the port.
	    unsigned prts_vector_width = 0;

	    for (unsigned inst = 0 ;  inst < instance.count() ;  inst += 1) {
		  NetScope*inst_scope = instance[inst];

		    // Scan the module sub-ports for this instance...
		  for (unsigned ldx = 0 ;  ldx < mport.count() ;  ldx += 1) {
			unsigned lbase = inst * mport.count();
			PEIdent*pport = mport[ldx];
			assert(pport);
			prts[lbase + ldx]
			      = pport->elaborate_port(des, inst_scope);
			if (prts[lbase + ldx] == 0)
			      continue;

			assert(prts[lbase + ldx]);
			prts_vector_width += prts[lbase + ldx]->vector_width();
		  }
	    }

	      // If I find that the port is unconnected inside the
	      // module, then there is nothing to connect. Skip the
	      // argument.
	    if (prts_vector_width == 0) {
		  continue;
	    }

	      // We know by design that each instance has the same
	      // width port. Therefore, the prts_pin_count must be an
	      // even multiple of the instance count.
	    assert(prts_vector_width % instance.count() == 0);

	    unsigned desired_vector_width = prts_vector_width;
	    if (instance.count() != 1)
		  desired_vector_width = 0;

	      // Elaborate the expression that connects to the
	      // module[s] port. sig is the thing outside the module
	      // that connects to the port.

	    NetNet*sig;
	    if ((prts.count() == 0)
		|| (prts[0]->port_type() == NetNet::PINPUT)) {

		    /* Input to module. elaborate the expression to
		       the desired width. If this in an instance
		       array, then let the net determine it's own
		       width. We use that, then, to decide how to hook
		       it up.

		       NOTE that this also handles the case that the
		       port is actually empty on the inside. We assume
		       in that case that the port is input. */

		  sig = pins[idx]->elaborate_net(des, scope,
						 desired_vector_width,
						 0, 0, 0);
		  if (sig == 0) {
			cerr << pins[idx]->get_line()
			     << ": internal error: Port expression "
			     << "too complicated for elaboration." << endl;
			continue;
		  }

	    } else if (prts[0]->port_type() == NetNet::PINOUT) {

		    /* Inout to/from module. This is a more
		       complicated case, where the expression must be
		       an lnet, but also an r-value net.

		       Normally, this winds up being the same as if we
		       just elaborated as an lnet, as passing a simple
		       identifier elaborates to the same NetNet in
		       both cases so the extra elaboration has no
		       effect. But if the expression passed to the
		       inout port is a part select, aspecial part
		       select must be created that can paqss data in
		       both directions.

		       Use the elaborate_bi_net method to handle all
		       the possible cases. */

		  sig = pins[idx]->elaborate_bi_net(des, scope);
		  if (sig == 0) {
			cerr << pins[idx]->get_line() << ": error: "
			     << "Inout port expression must support "
			     << "continuous assignment." << endl;
			cerr << pins[idx]->get_line() << ":      : "
			     << "Port of " << rmod->mod_name()
			     << " is " << rmod->ports[idx]->name << endl;
			des->errors += 1;
			continue;
		  }


	    } else {

		    /* Port type must be OUTPUT here. */

		    /* Output from module. Elaborate the port
		       expression as the l-value of a continuous
		       assignment, as the port will continuous assign
		       into the port. */

		  sig = pins[idx]->elaborate_lnet(des, scope, true);
		  if (sig == 0) {
			cerr << pins[idx]->get_line() << ": error: "
			     << "Output port expression must support "
			     << "continuous assignment." << endl;
			cerr << pins[idx]->get_line() << ":      : "
			     << "Port of " << rmod->mod_name()
			     << " is " << rmod->ports[idx]->name << endl;
			des->errors += 1;
			continue;
		  }

	    }

	    assert(sig);

#ifndef NDEBUG
	    if ((prts.count() >= 1)
		&& (prts[0]->port_type() != NetNet::PINPUT)) {
		  assert(sig->type() != NetNet::REG);
	    }
#endif

	      /* If we are working with an instance array, then the
		 signal width must match the port width exactly. */
	    if ((instance.count() != 1)
		&& (sig->vector_width() != prts_vector_width)
		&& (sig->vector_width() != prts_vector_width/instance.count())) {
		  cerr << pins[idx]->get_line() << ": error: "
		       << "Port expression width " << sig->vector_width()
		       << " does not match expected width "<< prts_vector_width
		       << " or " << (prts_vector_width/instance.count())
		       << "." << endl;
		  des->errors += 1;
		  continue;
	    }

	    if (debug_elaborate) {
		  cerr << get_line() << ": debug: " << get_name()
		       << ": Port " << idx << " has vector width of "
		       << prts_vector_width << "." << endl;
	    }

	      // Check that the parts have matching pin counts. If
	      // not, they are different widths. Note that idx is 0
	      // based, but users count parameter positions from 1.
	    if ((instance.count() == 1)
		&& (prts_vector_width != sig->vector_width())) {
		  cerr << get_line() << ": warning: Port " << (idx+1)
		       << " (" << rmod->ports[idx]->name << ") of "
		       << type_ << " expects " << prts_vector_width <<
			" bits, got " << sig->vector_width() << "." << endl;

		  if (prts_vector_width > sig->vector_width()) {
			cerr << get_line() << ":        : Leaving "
			     << (prts_vector_width-sig->vector_width())
			     << " high bits of the port unconnected."
			     << endl;
		  } else {
			cerr << get_line() << ":        : Leaving "
			     << (sig->vector_width()-prts_vector_width)
			     << " high bits of the expression dangling."
			     << endl;
		  }
	    }

	      // Connect the sig expression that is the context of the
	      // module instance to the ports of the elaborated module.

	      // The prts_pin_count variable is the total width of the
	      // port and is the maximum number of connections to
	      // make. sig is the elaborated expression that connects
	      // to that port. If sig has too few pins, then reduce
	      // the number of connections to make.

	      // Connect this many of the port pins. If the expression
	      // is too small, then reduce the number of connects.
	    unsigned ccount = prts_vector_width;
	    if (instance.count() == 1 && sig->vector_width() < ccount)
		  ccount = sig->vector_width();

	      // The spin_modulus is the width of the signal (not the
	      // port) if this is an instance array. This causes
	      // signals wide enough for a single instance to be
	      // connected to all the instances.
	    unsigned spin_modulus = prts_vector_width;
	    if (instance.count() != 1)
		  spin_modulus = sig->vector_width();

	      // Now scan the concatenation that makes up the port,
	      // connecting pins until we run out of port pins or sig
	      // pins. The sig object is the NetNet that is connected
	      // to the port from the outside, and the prts object is
	      // an array of signals to be connected to the sig.

	    NetConcat*ctmp;
	    unsigned spin = 0;

	    if (prts.count() == 1) {

		    // The simplest case, there are no
		    // parts/concatenations on the inside of the
		    // module, so the port and sig need simply be
		    // connected directly.
		  connect(prts[0]->pin(0), sig->pin(0));

	    } else if (sig->vector_width()==prts_vector_width/instance.count()
		       && prts.count()/instance.count() == 1) {

		  if (debug_elaborate){
			cerr << get_line() << ": debug: " << get_name()
			     << ": Replicating " << prts_vector_width
			     << " bits across all "
			     << prts_vector_width/instance.count()
			     << " sub-ports." << endl;
		  }

		    // The signal width is exactly the width of a
		    // single instance of the port. In this case,
		    // connect the sig to all the ports identically.
		  for (unsigned ldx = 0 ;  ldx < prts.count() ;  ldx += 1)
			connect(prts[ldx]->pin(0), sig->pin(0));

	    } else switch (prts[0]->port_type()) {
		case NetNet::POUTPUT:
		  ctmp = new NetConcat(scope, scope->local_symbol(),
				       prts_vector_width,
				       prts.count());
		  des->add_node(ctmp);
		  connect(ctmp->pin(0), sig->pin(0));
		  for (unsigned ldx = 0 ;  ldx < prts.count() ;  ldx += 1) {
			connect(ctmp->pin(ldx+1),
				prts[prts.count()-ldx-1]->pin(0));
		  }
		  break;

		case NetNet::PINPUT:
		  if (debug_elaborate){
			cerr << get_line() << ": debug: " << get_name()
			     << ": Dividing " << prts_vector_width
			     << " bits across all "
			     << prts_vector_width/instance.count()
			     << " input sub-ports of port "
			     << idx << "." << endl;
		  }

		  for (unsigned ldx = 0 ;  ldx < prts.count() ;  ldx += 1) {
			NetNet*sp = prts[prts.count()-ldx-1];
			NetPartSelect*ptmp = new NetPartSelect(sig, spin,
							   sp->vector_width(),
							   NetPartSelect::VP);
			des->add_node(ptmp);
			connect(ptmp->pin(0), sp->pin(0));
			spin += sp->vector_width();
		  }
		  break;
		case NetNet::PINOUT:
		  cerr << get_line() << ": XXXX: "
		       << "Forgot how to bind inout ports!" << endl;
		  des->errors += 1;
		  break;
		case NetNet::PIMPLICIT:
		  cerr << get_line() << ": internal error: "
		       << "Unexpected IMPLICIT port" << endl;
		  des->errors += 1;
		  break;
		case NetNet::NOT_A_PORT:
		  cerr << get_line() << ": internal error: "
		       << "Unexpected NOT_A_PORT port." << endl;
		  des->errors += 1;
		  break;
	    }

      }

}

/*
 * From a UDP definition in the source, make a NetUDP
 * object. Elaborate the pin expressions as netlists, then connect
 * those networks to the pins.
 */

void PGModule::elaborate_udp_(Design*des, PUdp*udp, NetScope*scope) const
{
      NetExpr*rise_expr =0, *fall_expr =0, *decay_expr =0;

      perm_string my_name = get_name();
      if (my_name == 0)
	    my_name = scope->local_symbol();

	/* When the parser notices delay expressions in front of a
	   module or primitive, it interprets them as parameter
	   overrides. Correct that misconception here. */
      if (overrides_) {
	    PDelays tmp_del;
	    tmp_del.set_delays(overrides_, false);
	    tmp_del.eval_delays(des, scope, rise_expr, fall_expr, decay_expr);

	    if (dynamic_cast<NetEConst*> (rise_expr)) {

	    } else {
		  cerr << get_line() << ": error: Delay expressions must be "
		       << "constant for primitives." << endl;
		  cerr << get_line() << ":      : Cannot calculate "
		       << *rise_expr << endl;
		  des->errors += 1;
	    }

	    if (dynamic_cast<NetEConst*> (fall_expr)) {

	    } else {
		  cerr << get_line() << ": error: Delay expressions must be "
		       << "constant for primitives." << endl;
		  cerr << get_line() << ":      : Cannot calculate "
		       << *rise_expr << endl;
		  des->errors += 1;
	    }

	    if (dynamic_cast<NetEConst*> (decay_expr)) {

	    } else {
		  cerr << get_line() << ": error: Delay expressions must be "
		       << "constant for primitives." << endl;
		  cerr << get_line() << ":      : Cannot calculate "
		       << *rise_expr << endl;
		  des->errors += 1;
	    }

      }


      assert(udp);
      NetUDP*net = new NetUDP(scope, my_name, udp->ports.count(), udp);
      net->rise_time(rise_expr);
      net->fall_time(fall_expr);
      net->decay_time(decay_expr);

      struct attrib_list_t*attrib_list = 0;
      unsigned attrib_list_n = 0;
      attrib_list = evaluate_attributes(attributes, attrib_list_n,
					des, scope);

      for (unsigned adx = 0 ;  adx < attrib_list_n ;  adx += 1)
	    net->attribute(attrib_list[adx].key, attrib_list[adx].val);

      delete[]attrib_list;


	// This is the array of pin expressions, shuffled to match the
	// order of the declaration. If the source instantiation uses
	// bind by order, this is the same as the source
	// list. Otherwise, the source list is rearranged by name
	// binding into this list.
      svector<PExpr*>pins;

	// Detect binding by name. If I am binding by name, then make
	// up a pins array that reflects the positions of the named
	// ports. If this is simply positional binding in the first
	// place, then get the binding from the base class.
      if (pins_) {
	    unsigned nexp = udp->ports.count();
	    pins = svector<PExpr*>(nexp);

	      // Scan the bindings, matching them with port names.
	    for (unsigned idx = 0 ;  idx < npins_ ;  idx += 1) {

		    // Given a binding, look at the module port names
		    // for the position that matches the binding name.
		  unsigned pidx = udp->find_port(pins_[idx].name);

		    // If the port name doesn't exist, the find_port
		    // method will return the port count. Detect that
		    // as an error.
		  if (pidx == nexp) {
			cerr << get_line() << ": error: port ``" <<
			      pins_[idx].name << "'' is not a port of "
			     << get_name() << "." << endl;
			des->errors += 1;
			continue;
		  }

		    // If I already bound something to this port, then
		    // the (*exp) array will already have a pointer
		    // value where I want to place this expression.
		  if (pins[pidx]) {
			cerr << get_line() << ": error: port ``" <<
			      pins_[idx].name << "'' already bound." <<
			      endl;
			des->errors += 1;
			continue;
		  }

		    // OK, do the binding by placing the expression in
		    // the right place.
		  pins[pidx] = pins_[idx].parm;
	    }

      } else {

	      /* Otherwise, this is a positional list of port
		 connections. In this case, the port count must be
		 right. Check that is is, the get the pin list. */

	    if (pin_count() != udp->ports.count()) {
		  cerr << get_line() << ": error: Wrong number "
			"of ports. Expecting " << udp->ports.count() <<
			", got " << pin_count() << "."
		       << endl;
		  des->errors += 1;
		  return;
	    }

	      // No named bindings, just use the positional list I
	      // already have.
	    assert(pin_count() == udp->ports.count());
	    pins = get_pins();
      }


	/* Handle the output port of the primitive special. It is an
	   output port (the only output port) so must be passed an
	   l-value net. */
      if (pins[0] == 0) {
	    cerr << get_line() << ": warning: output port unconnected."
		 << endl;

      } else {
	    NetNet*sig = pins[0]->elaborate_lnet(des, scope, true);
	    if (sig == 0) {
		  cerr << get_line() << ": error: "
		       << "Output port expression is not valid." << endl;
		  cerr << get_line() << ":      : Output "
		       << "port of " << udp->name_
		       << " is " << udp->ports[0] << "." << endl;
		  des->errors += 1;
	    } else {
		  connect(sig->pin(0), net->pin(0));
	    }
      }

	/* Run through the pins, making netlists for the pin
	   expressions and connecting them to the pin in question. All
	   of this is independent of the nature of the UDP. */
      for (unsigned idx = 1 ;  idx < net->pin_count() ;  idx += 1) {
	    if (pins[idx] == 0)
		  continue;

	    NetNet*sig = pins[idx]->elaborate_net(des, scope, 1, 0, 0, 0);
	    if (sig == 0) {
		  cerr << "internal error: Expression too complicated "
			"for elaboration:" << pins[idx] << endl;
		  continue;
	    }

	    connect(sig->pin(0), net->pin(idx));
      }

	// All done. Add the object to the design.
      des->add_node(net);
}


bool PGModule::elaborate_sig(Design*des, NetScope*scope) const
{
	// Look for the module type
      map<perm_string,Module*>::const_iterator mod = pform_modules.find(type_);
      if (mod != pform_modules.end())
	    return elaborate_sig_mod_(des, scope, (*mod).second);

      return true;
}


void PGModule::elaborate(Design*des, NetScope*scope) const
{
	// Look for the module type
      map<perm_string,Module*>::const_iterator mod = pform_modules.find(type_);
      if (mod != pform_modules.end()) {
	    elaborate_mod_(des, (*mod).second, scope);
	    return;
      }

	// Try a primitive type
      map<perm_string,PUdp*>::const_iterator udp = pform_primitives.find(type_);
      if (udp != pform_primitives.end()) {
	    assert((*udp).second);
	    elaborate_udp_(des, (*udp).second, scope);
	    return;
      }

      cerr << get_line() << ": internal error: Unknown module type: " <<
	    type_ << endl;
}

void PGModule::elaborate_scope(Design*des, NetScope*sc) const
{
	// Look for the module type
      map<perm_string,Module*>::const_iterator mod = pform_modules.find(type_);
      if (mod != pform_modules.end()) {
	    elaborate_scope_mod_(des, (*mod).second, sc);
	    return;
      }

	// Try a primitive type
      map<perm_string,PUdp*>::const_iterator udp = pform_primitives.find(type_);
      if (udp != pform_primitives.end())
	    return;

	// Not a module or primitive that I know about yet, so try to
	// load a library module file (which parses some new Verilog
	// code) and try again.
      if (load_module(type_)) {

	      // Try again to find the module type
	    mod = pform_modules.find(type_);
	    if (mod != pform_modules.end()) {
		  elaborate_scope_mod_(des, (*mod).second, sc);
		  return;
	    }

	      // Try again to find a primitive type
	    udp = pform_primitives.find(type_);
	    if (udp != pform_primitives.end())
		  return;
      }


	// Not a module or primitive that I know about or can find by
	// any means, so give up.
      cerr << get_line() << ": error: Unknown module type: " << type_ << endl;
      missing_modules[type_] += 1;
      des->errors += 1;
}


NetProc* Statement::elaborate(Design*des, NetScope*) const
{
      cerr << get_line() << ": internal error: elaborate: "
	    "What kind of statement? " << typeid(*this).name() << endl;
      NetProc*cur = new NetProc;
      des->errors += 1;
      return cur;
}


NetAssign_* PAssign_::elaborate_lval(Design*des, NetScope*scope) const
{
      assert(lval_);
      return lval_->elaborate_lval(des, scope, false);
}

/*
 * This function elaborates delay expressions. This is a little
 * different from normal elaboration because the result may need to be
 * scaled.
 */
static NetExpr*elaborate_delay_expr(PExpr*expr, Design*des, NetScope*scope)
{
      NetExpr*dex = elab_and_eval(des, scope, expr, -1);

	/* If the delay expression is a real constant or vector
	   constant, then evaluate it, scale it to the local time
	   units, and return an adjusted NetEConst. */

      if (NetECReal*tmp = dynamic_cast<NetECReal*>(dex)) {
	    verireal fn = tmp->value();

	    int shift = scope->time_unit() - des->get_precision();
	    int64_t delay = fn.as_long64(shift);
	    if (delay < 0)
		  delay = 0;

	    delete tmp;
	    return new NetEConst(verinum(delay));
      }


      if (NetEConst*tmp = dynamic_cast<NetEConst*>(dex)) {
	    verinum fn = tmp->value();

	    uint64_t delay =
		  des->scale_to_precision(fn.as_ulong64(), scope);

	    delete tmp;
	    return new NetEConst(verinum(delay));
      }


	/* The expression is not constant, so generate an expanded
	   expression that includes the necessary scale shifts, and
	   return that expression. */
      int shift = scope->time_unit() - des->get_precision();
      if (shift > 0) {
	    uint64_t scale = 1;
	    while (shift > 0) {
		  scale *= 10;
		  shift -= 1;
	    }

	    NetExpr*scal_val = new NetEConst(verinum(scale));
	    dex = new NetEBMult('*', dex, scal_val);
      }

      if (shift < 0) {
	    unsigned long scale = 1;
	    while (shift < 0) {
		  scale *= 10;
		  shift += 1;
	    }

	    NetExpr*scal_val = new NetEConst(verinum(scale));
	    dex = new NetEBDiv('/', dex, scal_val);
      }

      return dex;
}

NetProc* PAssign::elaborate(Design*des, NetScope*scope) const
{
      assert(scope);

	/* elaborate the lval. This detects any part selects and mux
	   expressions that might exist. */
      NetAssign_*lv = elaborate_lval(des, scope);
      if (lv == 0) return 0;

	/* If there is an internal delay expression, elaborate it. */
      NetExpr*delay = 0;
      if (delay_ != 0)
	    delay = elaborate_delay_expr(delay_, des, scope);


      assert(rval());

	/* Elaborate the r-value expression, then try to evaluate it. */

	/* Find out what the r-value width is going to be. We guess it
	   will be the l-value width, but it may turn out to be
	   something else based on self-determined widths inside. */
      unsigned use_width = lv->lwidth();
      bool unsized_flag = false;
      use_width = rval()->test_width(des, scope, use_width, use_width, unsized_flag);

	/* Now elaborate to the expected width. */
      NetExpr*rv = elab_and_eval(des, scope, rval(), use_width);
      if (rv == 0) return 0;
      assert(rv);


	/* Rewrite delayed assignments as assignments that are
	   delayed. For example, a = #<d> b; becomes:

	     begin
	        tmp = b;
		#<d> a = tmp;
	     end

	   If the delay is an event delay, then the transform is
	   similar, with the event delay replacing the time delay. It
	   is an event delay if the event_ member has a value.

	   This rewriting of the expression allows me to not bother to
	   actually and literally represent the delayed assign in the
	   netlist. The compound statement is exactly equivalent. */

      if (delay || event_) {
	    unsigned wid = lv->lwidth();

	    rv->set_width(wid);
	    rv = pad_to_width(rv, wid);

	    if (wid > rv->expr_width()) {
		  cerr << get_line() << ": error: Unable to match "
			"expression width of " << rv->expr_width() <<
			" to l-value width of " << wid << "." << endl;
		    //XXXX delete rv;
		  return 0;
	    }

	    NetNet*tmp = new NetNet(scope, scope->local_symbol(),
				    NetNet::REG, wid);
	    tmp->set_line(*this);
	    tmp->data_type(rv->expr_type());

	    NetESignal*sig = new NetESignal(tmp);

	      /* Generate an assignment of the l-value to the temporary... */
	    string n = scope->local_hsymbol();
	    NetAssign_*lvt = new NetAssign_(tmp);

	    NetAssign*a1 = new NetAssign(lvt, rv);
	    a1->set_line(*this);

	      /* Generate an assignment of the temporary to the r-value... */
	    NetAssign*a2 = new NetAssign(lv, sig);
	    a2->set_line(*this);

	      /* Generate the delay statement with the final
		 assignment attached to it. If this is an event delay,
		 elaborate the PEventStatement. Otherwise, create the
		 right NetPDelay object. */
	    NetProc*st;
	    if (event_) {
		  st = event_->elaborate_st(des, scope, a2);
		  if (st == 0) {
			cerr << event_->get_line() << ": error: "
			      "unable to elaborate event expression."
			     << endl;
			des->errors += 1;
			return 0;
		  }
		  assert(st);

	    } else {
		  NetPDelay*de = new NetPDelay(delay, a2);
		  de->set_line(*this);
		  st = de;
	    }

	      /* And build up the complex statement. */
	    NetBlock*bl = new NetBlock(NetBlock::SEQU, 0);
	    bl->append(a1);
	    bl->append(st);

	    return bl;
      }

	/* Based on the specific type of the l-value, do cleanup
	   processing on the r-value. */
      if (rv->expr_type() == IVL_VT_REAL) {

	      // The r-value is a real. Casting will happen in the
	      // code generator, so leave it.

      } else {
	    unsigned wid = count_lval_width(lv);
	    rv->set_width(wid);
	    rv = pad_to_width(rv, wid);
	    assert(rv->expr_width() >= wid);
      }

      NetAssign*cur = new NetAssign(lv, rv);
      cur->set_line(*this);

      return cur;
}

/*
 * Elaborate non-blocking assignments. The statement is of the general
 * form:
 *
 *    <lval> <= #<delay> <rval> ;
 */
NetProc* PAssignNB::elaborate(Design*des, NetScope*scope) const
{
      assert(scope);

	/* Elaborate the l-value. */
      NetAssign_*lv = elaborate_lval(des, scope);
      if (lv == 0) return 0;

      assert(rval());

	/* Elaborate and precalculate the r-value. */
      NetExpr*rv = elab_and_eval(des, scope, rval(), count_lval_width(lv));
      if (rv == 0)
	    return 0;

	/* Handle the (common) case that the r-value is a vector. This
	   includes just about everything but reals. In this case, we
	   need to pad the r-value to match the width of the l-value.

	   If in this case the l-val is a variable (i.e. real) then
	   the width to pad to will be 0, so this code is harmless. */
      if (rv->expr_type() == IVL_VT_REAL) {

      } else {
	    unsigned wid = count_lval_width(lv);
	    rv->set_width(wid);
	    rv = pad_to_width(rv, wid);
      }

      NetExpr*delay = 0;
      if (delay_ != 0)
	    delay = elaborate_delay_expr(delay_, des, scope);

	/* All done with this node. Mark its line number and check it in. */
      NetAssignNB*cur = new NetAssignNB(lv, rv);
      cur->set_delay(delay);
      cur->set_line(*this);
      return cur;
}


/*
 * This is the elaboration method for a begin-end block. Try to
 * elaborate the entire block, even if it fails somewhere. This way I
 * get all the error messages out of it. Then, if I detected a failure
 * then pass the failure up.
 */
NetProc* PBlock::elaborate(Design*des, NetScope*scope) const
{
      assert(scope);

      NetBlock::Type type = (bl_type_==PBlock::BL_PAR)
	    ? NetBlock::PARA
	    : NetBlock::SEQU;

      NetScope*nscope = 0;
      if (name_.str() != 0) {
	    nscope = scope->child(name_);
	    if (nscope == 0) {
		  cerr << get_line() << ": internal error: "
			"unable to find block scope " << scope->name()
		       << "<" << name_ << ">" << endl;
		  des->errors += 1;
		  return 0;
	    }

	    assert(nscope);

      }

      NetBlock*cur = new NetBlock(type, nscope);
      bool fail_flag = false;

      if (nscope == 0)
	    nscope = scope;

	// Handle the special case that the block contains only one
	// statement. There is no need to keep the block node. Also,
	// don't elide named blocks, because they might be referenced
	// elsewhere.
      if ((list_.count() == 1) && (name_.str() == 0)) {
	    assert(list_[0]);
	    NetProc*tmp = list_[0]->elaborate(des, nscope);
	    return tmp;
      }

      for (unsigned idx = 0 ;  idx < list_.count() ;  idx += 1) {
	    assert(list_[idx]);
	    NetProc*tmp = list_[idx]->elaborate(des, nscope);
	    if (tmp == 0) {
		  fail_flag = true;
		  continue;
	    }

	      // If the result turns out to be a noop, then skip it.
	    if (NetBlock*tbl = dynamic_cast<NetBlock*>(tmp))
		  if (tbl->proc_first() == 0) {
			delete tbl;
			continue;
		  }

	    cur->append(tmp);
      }

      if (fail_flag) {
	    delete cur;
	    cur = 0;
      }

      return cur;
}

/*
 * Elaborate a case statement.
 */
NetProc* PCase::elaborate(Design*des, NetScope*scope) const
{
      assert(scope);

      NetExpr*expr = elab_and_eval(des, scope, expr_, -1);
      if (expr == 0) {
	    cerr << get_line() << ": error: Unable to elaborate this case"
		  " expression." << endl;
	    return 0;
      }

	/* Count the items in the case statement. Note that there may
	   be some cases that have multiple guards. Count each as a
	   separate item. */
      unsigned icount = 0;
      for (unsigned idx = 0 ;  idx < items_->count() ;  idx += 1) {
	    PCase::Item*cur = (*items_)[idx];

	    if (cur->expr.count() == 0)
		  icount += 1;
	    else
		  icount += cur->expr.count();
      }

      NetCase*res = new NetCase(type_, expr, icount);
      res->set_line(*this);

	/* Iterate over all the case items (guard/statement pairs)
	   elaborating them. If the guard has no expression, then this
	   is a "default" cause. Otherwise, the guard has one or more
	   expressions, and each guard is a case. */
      unsigned inum = 0;
      for (unsigned idx = 0 ;  idx < items_->count() ;  idx += 1) {

	    assert(inum < icount);
	    PCase::Item*cur = (*items_)[idx];

	    if (cur->expr.count() == 0) {
		    /* If there are no expressions, then this is the
		       default case. */
		  NetProc*st = 0;
		  if (cur->stat)
			st = cur->stat->elaborate(des, scope);

		  res->set_case(inum, 0, st);
		  inum += 1;

	    } else for (unsigned e = 0; e < cur->expr.count(); e += 1) {

		    /* If there are one or more expressions, then
		       iterate over the guard expressions, elaborating
		       a separate case for each. (Yes, the statement
		       will be elaborated again for each.) */
		  NetExpr*gu = 0;
		  NetProc*st = 0;
		  assert(cur->expr[e]);
		  gu = elab_and_eval(des, scope, cur->expr[e], -1);

		  if (cur->stat)
			st = cur->stat->elaborate(des, scope);

		  res->set_case(inum, gu, st);
		  inum += 1;
	    }
      }

      return res;
}

NetProc* PCondit::elaborate(Design*des, NetScope*scope) const
{
      assert(scope);

	// Elaborate and try to evaluate the conditional expression.
      NetExpr*expr = elab_and_eval(des, scope, expr_, -1);
      if (expr == 0) {
	    cerr << get_line() << ": error: Unable to elaborate"
		  " condition expression." << endl;
	    des->errors += 1;
	    return 0;
      }

	// If the condition of the conditional statement is constant,
	// then look at the value and elaborate either the if statement
	// or the else statement. I don't need both. If there is no
	// else_ statement, the use an empty block as a noop.
      if (NetEConst*ce = dynamic_cast<NetEConst*>(expr)) {
	    verinum val = ce->value();
	    if (debug_elaborate) {
		  cerr << get_line() << ": debug: Condition expression "
		       << "is a constant " << val << "." << endl;
	    }

	    verinum::V reduced = verinum::V0;
	    for (unsigned idx = 0 ;  idx < val.len() ;  idx += 1)
		  reduced = reduced | val[idx];

	    delete expr;
	    if (reduced == verinum::V1)
		  if (if_) {
			return if_->elaborate(des, scope);
		  } else {
			NetBlock*tmp = new NetBlock(NetBlock::SEQU, 0);
			tmp->set_line(*this);
			return tmp;
		  }
	    else if (else_)
		  return else_->elaborate(des, scope);
	    else
		  return new NetBlock(NetBlock::SEQU, 0);
      }

	// If the condition expression is more then 1 bits, then
	// generate a comparison operator to get the result down to
	// one bit. Turn <e> into <e> != 0;

      if (expr->expr_width() < 1) {
	    cerr << get_line() << ": internal error: "
		  "incomprehensible expression width (0)." << endl;
	    return 0;
      }

      if (expr->expr_width() > 1) {
	    assert(expr->expr_width() > 1);
	    verinum zero (verinum::V0, expr->expr_width());
	    NetEConst*ezero = new NetEConst(zero);
	    ezero->set_width(expr->expr_width());
	    NetEBComp*cmp = new NetEBComp('n', expr, ezero);
	    expr = cmp;
      }

	// Well, I actually need to generate code to handle the
	// conditional, so elaborate.
      NetProc*i = if_? if_->elaborate(des, scope) : 0;
      NetProc*e = else_? else_->elaborate(des, scope) : 0;

	// Detect the special cases that the if or else statements are
	// empty blocks. If this is the case, remove the blocks as
	// null statements.
      if (NetBlock*tmp = dynamic_cast<NetBlock*>(i)) {
	    if (tmp->proc_first() == 0) {
		  delete i;
		  i = 0;
	    }
      }

      if (NetBlock*tmp = dynamic_cast<NetBlock*>(e)) {
	    if (tmp->proc_first() == 0) {
		  delete e;
		  e = 0;
	    }
      }

      NetCondit*res = new NetCondit(expr, i, e);
      res->set_line(*this);
      return res;
}

NetProc* PCallTask::elaborate(Design*des, NetScope*scope) const
{
      if (path_.peek_name(0)[0] == '$')
	    return elaborate_sys(des, scope);
      else
	    return elaborate_usr(des, scope);
}

/*
 * A call to a system task involves elaborating all the parameters,
 * then passing the list to the NetSTask object.
 *XXXX
 * There is a single special case in the call to a system
 * task. Normally, an expression cannot take an unindexed
 * memory. However, it is possible to take a system task parameter a
 * memory if the expression is trivial.
 */
NetProc* PCallTask::elaborate_sys(Design*des, NetScope*scope) const
{
      assert(scope);

      unsigned parm_count = nparms();

	/* Catch the special case that the system task has no
	   parameters. The "()" string will be parsed as a single
	   empty parameter, when we really mean no parameters at all. */
      if ((nparms() == 1) && (parm(0) == 0))
	    parm_count = 0;

      svector<NetExpr*>eparms (parm_count);

      for (unsigned idx = 0 ;  idx < parm_count ;  idx += 1) {
	    PExpr*ex = parm(idx);
	    eparms[idx] = ex? ex->elaborate_expr(des, scope, -1, true) : 0;

	      /* Attempt to pre-evaluate the parameters. It may be
		 possible to at least partially reduce the
		 expression. */
	    if (eparms[idx] && !dynamic_cast<NetEConst*>(eparms[idx])) {
		  NetExpr*tmp = eparms[idx]->eval_tree();
		  if (tmp != 0) {
			delete eparms[idx];
			eparms[idx] = tmp;
		  }
	    }
      }

      NetSTask*cur = new NetSTask(path_.peek_name(0), eparms);
      return cur;
}

/*
 * A call to a user defined task is different from a call to a system
 * task because a user task in a netlist has no parameters: the
 * assignments are done by the calling thread. For example:
 *
 *  task foo;
 *    input a;
 *    output b;
 *    [...]
 *  endtask;
 *
 *  [...] foo(x, y);
 *
 * is really:
 *
 *  task foo;
 *    reg a;
 *    reg b;
 *    [...]
 *  endtask;
 *
 *  [...]
 *  begin
 *    a = x;
 *    foo;
 *    y = b;
 *  end
 */
NetProc* PCallTask::elaborate_usr(Design*des, NetScope*scope) const
{
      assert(scope);

      NetScope*task = des->find_task(scope, path_);
      if (task == 0) {
	    cerr << get_line() << ": error: Enable of unknown task "
		 << "``" << path_ << "''." << endl;
	    des->errors += 1;
	    return 0;
      }

      assert(task);
      assert(task->type() == NetScope::TASK);
      NetTaskDef*def = task->task_def();
      if (def == 0) {
	    cerr << get_line() << ": internal error: task " << path_
		 << " doesn't have a definition in " << scope->name()
		 << "." << endl;
	    des->errors += 1;
	    return 0;
      }
      assert(def);

      if (nparms() != def->port_count()) {
	    cerr << get_line() << ": error: Port count mismatch in call to ``"
		 << path_ << "''." << endl;
	    des->errors += 1;
	    return 0;
      }

      NetUTask*cur;

	/* Handle tasks with no parameters specially. There is no need
	   to make a sequential block to hold the generated code. */
      if (nparms() == 0) {
	    cur = new NetUTask(task);
	    return cur;
      }

      NetBlock*block = new NetBlock(NetBlock::SEQU, 0);


	/* Detect the case where the definition of the task is known
	   empty. In this case, we need not bother with calls to the
	   task, all the assignments, etc. Just return a no-op. */

      if (const NetBlock*tp = dynamic_cast<const NetBlock*>(def->proc())) {
	    if (tp->proc_first() == 0)
		  return block;
      }

	/* Generate assignment statement statements for the input and
	   INOUT ports of the task. These are managed by writing
	   assignments with the task port the l-value and the passed
	   expression the r-value. We know by definition that the port
	   is a reg type, so this elaboration is pretty obvious. */

      for (unsigned idx = 0 ;  idx < nparms() ;  idx += 1) {

	    NetNet*port = def->port(idx);
	    assert(port->port_type() != NetNet::NOT_A_PORT);
	    if (port->port_type() == NetNet::POUTPUT)
		  continue;

	    NetAssign_*lv = new NetAssign_(port);
	    unsigned wid = count_lval_width(lv);

	    NetExpr*rv = elab_and_eval(des, scope, parms_[idx], wid);
	    rv->set_width(wid);
	    rv = pad_to_width(rv, wid);
	    NetAssign*pr = new NetAssign(lv, rv);
	    block->append(pr);
      }

	/* Generate the task call proper... */
      cur = new NetUTask(task);
      block->append(cur);


	/* Generate assignment statements for the output and INOUT
	   ports of the task. The l-value in this case is the
	   expression passed as a parameter, and the r-value is the
	   port to be copied out.

	   We know by definition that the r-value of this copy-out is
	   the port, which is a reg. The l-value, however, may be any
	   expression that can be a target to a procedural
	   assignment, including a memory word. */

      for (unsigned idx = 0 ;  idx < nparms() ;  idx += 1) {

	    NetNet*port = def->port(idx);

	      /* Skip input ports. */
	    assert(port->port_type() != NetNet::NOT_A_PORT);
	    if (port->port_type() == NetNet::PINPUT)
		  continue;


	      /* Elaborate an l-value version of the port expression
		 for output and inout ports. If the expression does
		 not exist then quietly skip it, but if the expression
		 is not a valid l-value print an error message. Note
		 that the elaborate_lval method already printed a
		 detailed message. */
	    NetAssign_*lv;
	    if (parms_[idx]) {
		  lv = parms_[idx]->elaborate_lval(des, scope, false);
		  if (lv == 0) {
			cerr << parms_[idx]->get_line() << ": error: "
			     << "I give up on task port " << (idx+1)
			     << " expression: " << *parms_[idx] << endl;
		  }
	    } else {
		  lv = 0;
	    }

	    if (lv == 0)
		  continue;

	    NetESignal*sig = new NetESignal(port);
	    NetExpr*rv = pad_to_width(sig, count_lval_width(lv));

	      /* Generate the assignment statement. */
	    NetAssign*ass = new NetAssign(lv, rv);

	    block->append(ass);
      }

      return block;
}

/*
 * Elaborate a procedural continuous assign. This really looks very
 * much like other procedural assignments, at this point, but there
 * is no delay to worry about. The code generator will take care of
 * the differences between continuous assign and normal assignments.
 */
NetCAssign* PCAssign::elaborate(Design*des, NetScope*scope) const
{
      NetCAssign*dev = 0;
      assert(scope);

      NetAssign_*lval = lval_->elaborate_lval(des, scope, false);
      if (lval == 0)
	    return 0;

      unsigned lwid = count_lval_width(lval);

      NetExpr*rexp = elab_and_eval(des, scope, expr_, lwid);
      if (rexp == 0)
	    return 0;

      rexp->set_width(lwid);
      rexp = pad_to_width(rexp, lwid);

      dev = new NetCAssign(lval, rexp);

      if (debug_elaborate) {
	    cerr << get_line() << ": debug: Elaborate cassign,"
		 << " lval width=" << lwid
		 << " rval width=" << rexp->expr_width()
		 << " rval=" << *rexp
		 << endl;
      }

      dev->set_line(*this);
      return dev;
}

NetDeassign* PDeassign::elaborate(Design*des, NetScope*scope) const
{
      assert(scope);

      NetAssign_*lval = lval_->elaborate_lval(des, scope, false);
      if (lval == 0)
	    return 0;

      NetDeassign*dev = new NetDeassign(lval);
      dev->set_line( *this );
      return dev;
}

/*
 * Elaborate the delay statement (of the form #<expr> <statement>) as a
 * NetPDelay object. If the expression is constant, evaluate it now
 * and make a constant delay. If not, then pass an elaborated
 * expression to the constructor of NetPDelay so that the code
 * generator knows to evaluate the expression at run time.
 */
NetProc* PDelayStatement::elaborate(Design*des, NetScope*scope) const
{
      assert(scope);

	/* This call evaluates the delay expression to a NetEConst, if
	   possible. This includes transforming NetECReal values to
	   integers, and applying the proper scaling. */
      NetExpr*dex = elaborate_delay_expr(delay_, des, scope);

      if (NetEConst*tmp = dynamic_cast<NetEConst*>(dex)) {
	    if (statement_)
		  return new NetPDelay(tmp->value().as_ulong64(),
				       statement_->elaborate(des, scope));
	    else
		  return new NetPDelay(tmp->value().as_ulong(), 0);

	    delete dex;

      } else {
	    if (statement_)
		  return new NetPDelay(dex, statement_->elaborate(des, scope));
	    else
		  return new NetPDelay(dex, 0);
      }

}

/*
 * The disable statement is not yet supported.
 */
NetProc* PDisable::elaborate(Design*des, NetScope*scope) const
{
      assert(scope);

      NetScope*target = des->find_scope(scope, scope_);
      if (target == 0) {
	    cerr << get_line() << ": error: Cannot find scope "
		 << scope_ << " in " << scope->name() << endl;
	    des->errors += 1;
	    return 0;
      }

      switch (target->type()) {
	  case NetScope::FUNC:
	    cerr << get_line() << ": error: Cannot disable functions." << endl;
	    des->errors += 1;
	    return 0;

	  case NetScope::MODULE:
	    cerr << get_line() << ": error: Cannot disable modules." << endl;
	    des->errors += 1;
	    return 0;

	  default:
	    break;
      }

      NetDisable*obj = new NetDisable(target);
      obj->set_line(*this);
      return obj;
}

/*
 * An event statement is an event delay of some sort, attached to a
 * statement. Some Verilog examples are:
 *
 *      @(posedge CLK) $display("clock rise");
 *      @event_1 $display("event triggered.");
 *      @(data or negedge clk) $display("data or clock fall.");
 *
 * The elaborated netlist uses the NetEvent, NetEvWait and NetEvProbe
 * classes. The NetEvWait class represents the part of the netlist
 * that is executed by behavioral code. The process starts waiting on
 * the NetEvent when it executes the NetEvWait step. Net NetEvProbe
 * and NetEvTrig are structural and behavioral equivalents that
 * trigger the event, and awakens any processes blocking in the
 * associated wait.
 *
 * The basic data structure is:
 *
 *       NetEvWait ---/--->  NetEvent  <----\---- NetEvProbe
 *        ...         |                     |         ...
 *       NetEvWait ---+                     +---- NetEvProbe
 *                                          |         ...
 *                                          +---- NetEvTrig
 *
 * That is, many NetEvWait statements may wait on a single NetEvent
 * object, and Many NetEvProbe objects may trigger the NetEvent
 * object. The many NetEvWait objects pointing to the NetEvent object
 * reflects the possibility of different places in the code blocking
 * on the same named event, like so:
 *
 *         event foo;
 *           [...]
 *         always begin @foo <statement1>; @foo <statement2> end
 *
 * This tends to not happen with signal edges. The multiple probes
 * pointing to the same event reflect the possibility of many
 * expressions in the same blocking statement, like so:
 *
 *         wire reset, clk;
 *           [...]
 *         always @(reset or posedge clk) <stmt>;
 *
 * Conjunctions like this cause a NetEvent object be created to
 * represent the overall conjunction, and NetEvProbe objects for each
 * event expression.
 *
 * If the NetEvent object represents a named event from the source,
 * then there are NetEvTrig objects that represent the trigger
 * statements instead of the NetEvProbe objects representing signals.
 * For example:
 *
 *         event foo;
 *         always @foo <stmt>;
 *         initial begin
 *                [...]
 *            -> foo;
 *                [...]
 *            -> foo;
 *                [...]
 *         end
 *
 * Each trigger statement in the source generates a separate NetEvTrig
 * object in the netlist. Those trigger objects are elaborated
 * elsewhere.
 *
 * Additional complications arise when named events show up in
 * conjunctions. An example of such a case is:
 *
 *         event foo;
 *         wire bar;
 *         always @(foo or posedge bar) <stmt>;
 *
 * Since there is by definition a NetEvent object for the foo object,
 * this is handled by allowing the NetEvWait object to point to
 * multiple NetEvent objects. All the NetEvProbe based objects are
 * collected and pointed as the synthetic NetEvent object, and all the
 * named events are added into the list of NetEvent object that the
 * NetEvWait object can refer to.
 */

NetProc* PEventStatement::elaborate_st(Design*des, NetScope*scope,
				       NetProc*enet) const
{
      assert(scope);

	/* Create a single NetEvent and NetEvWait. Then, create a
	   NetEvProbe for each conjunctive event in the event
	   list. The NetEvProbe objects all refer back to the NetEvent
	   object. */

      NetEvent*ev = new NetEvent(scope->local_symbol());
      ev->set_line(*this);
      unsigned expr_count = 0;

      NetEvWait*wa = new NetEvWait(enet);
      wa->set_line(*this);

	/* If there are no expressions, this is a signal that it is an
	   @* statement. Generate an expression to use. */

      if (expr_.count() == 0) {
	    assert(enet);
	    NexusSet*nset = enet->nex_input();
	    if (nset == 0) {
		  cerr << get_line() << ": internal error: No NexusSet"
		       << " from statement." << endl;
		  enet->dump(cerr, 6);
		  des->errors += 1;
		  return enet;
	    }

	    if (nset->count() == 0) {
		  cerr << get_line() << ": error: No inputs to statement."
		       << " The @* cannot execute." << endl;
		  des->errors += 1;
		  return enet;
	    }

	    NetEvProbe*pr = new NetEvProbe(scope, scope->local_symbol(),
					   ev, NetEvProbe::ANYEDGE,
					   nset->count());
	    for (unsigned idx = 0 ;  idx < nset->count() ;  idx += 1)
		  connect(nset[0][idx], pr->pin(idx));

	    delete nset;
	    des->add_node(pr);

	    expr_count = 1;

      } else for (unsigned idx = 0 ;  idx < expr_.count() ;  idx += 1) {

	    assert(expr_[idx]->expr());

	      /* If the expression is an identifier that matches a
		 named event, then handle this case all at once at
		 skip the rest of the expression handling. */

	    if (PEIdent*id = dynamic_cast<PEIdent*>(expr_[idx]->expr())) {
		  NetNet*       sig = 0;
		  NetMemory*    mem = 0;
		  const NetExpr*par = 0;
		  NetEvent*     eve = 0;

		  NetScope*found_in = symbol_search(des, scope, id->path(),
						    sig, mem, par, eve);

		  if (found_in && eve) {
			wa->add_event(eve);
			continue;
		  }
	    }


	      /* So now we have a normal event expression. Elaborate
		 the sub-expression as a net and decide how to handle
		 the edge. */

	    bool save_flag = error_implicit;
	    error_implicit = true;
	    NetNet*expr = expr_[idx]->expr()->elaborate_net(des, scope,
							    0, 0, 0, 0);
	    error_implicit = save_flag;
	    if (expr == 0) {
		  expr_[idx]->dump(cerr);
		  cerr << endl;
		  des->errors += 1;
		  continue;
	    }
	    assert(expr);

	    unsigned pins = (expr_[idx]->type() == PEEvent::ANYEDGE)
		  ? expr->pin_count() : 1;

	    NetEvProbe*pr;
	    switch (expr_[idx]->type()) {
		case PEEvent::POSEDGE:
		  pr = new NetEvProbe(scope, scope->local_symbol(), ev,
				      NetEvProbe::POSEDGE, pins);
		  break;

		case PEEvent::NEGEDGE:
		  pr = new NetEvProbe(scope, scope->local_symbol(), ev,
				      NetEvProbe::NEGEDGE, pins);
		  break;

		case PEEvent::ANYEDGE:
		  pr = new NetEvProbe(scope, scope->local_symbol(), ev,
				      NetEvProbe::ANYEDGE, pins);
		  break;

		default:
		  assert(0);
	    }

	    for (unsigned p = 0 ;  p < pr->pin_count() ; p += 1)
		  connect(pr->pin(p), expr->pin(p));

	    des->add_node(pr);
	    expr_count += 1;
      }

	/* If there was at least one conjunction that was an
	   expression (and not a named event) then add this
	   event. Otherwise, we didn't use it so delete it. */
      if (expr_count > 0) {
	    scope->add_event(ev);
	    wa->add_event(ev);
	      /* NOTE: This event that I am adding to the wait may be
		 a duplicate of another event somewhere else. However,
		 I don't know that until all the modules are hooked
		 up, so it is best to leave find_similar_event to
		 after elaboration. */
      } else {
	    delete ev;
      }

      return wa;
}

/*
 * This is the special case of the event statement, the wait
 * statement. This is elaborated into a slightly more complicated
 * statement that uses non-wait statements:
 *
 *     wait (<expr>)  <statement>
 *
 * becomes
 *
 *     begin
 *         while (1 !== <expr>)
 *           @(<expr inputs>) <noop>;
 *         <statement>;
 *     end
 */
NetProc* PEventStatement::elaborate_wait(Design*des, NetScope*scope,
					 NetProc*enet) const
{
      assert(scope);
      assert(expr_.count() == 1);

      const PExpr *pe = expr_[0]->expr();

	/* Elaborate wait expression. Don't eval yet, we will do that
	   shortly, after we apply a reduction or. */
      NetExpr*expr = pe->elaborate_expr(des, scope, -1, false);
      if (expr == 0) {
	    cerr << get_line() << ": error: Unable to elaborate"
		  " wait condition expression." << endl;
	    des->errors += 1;
	    return 0;
      }

	// If the condition expression is more then 1 bits, then
	// generate a reduction operator to get the result down to
	// one bit. In other words, Turn <e> into |<e>;

      if (expr->expr_width() < 1) {
	    cerr << get_line() << ": internal error: "
		  "incomprehensible wait expression width (0)." << endl;
	    return 0;
      }

      if (expr->expr_width() > 1) {
	    assert(expr->expr_width() > 1);
	    NetEUReduce*cmp = new NetEUReduce('|', expr);
	    expr = cmp;
      }

	/* precalculate as much as possible of the wait expression. */
      if (NetExpr*tmp = expr->eval_tree()) {
	    delete expr;
	    expr = tmp;
      }

	/* Detect the unusual case that the wait expression is
	   constant. Constant true is OK (it becomes transparent) but
	   constant false is almost certainly not what is intended. */
      assert(expr->expr_width() == 1);
      if (NetEConst*ce = dynamic_cast<NetEConst*>(expr)) {
	    verinum val = ce->value();
	    assert(val.len() == 1);

	      /* Constant true -- wait(1) <s1> reduces to <s1>. */
	    if (val[0] == verinum::V1) {
		  delete expr;
		  assert(enet);
		  return enet;
	    }

	      /* Otherwise, false. wait(0) blocks permanently. */

	    cerr << get_line() << ": warning: wait expression is "
		 << "constant false." << endl;
	    cerr << get_line() << ":        : The statement will "
		 << "block permanently." << endl;

	      /* Create an event wait and an otherwise unreferenced
		 event variable to force a perpetual wait. */
	    NetEvent*wait_event = new NetEvent(scope->local_symbol());
	    scope->add_event(wait_event);

	    NetEvWait*wait = new NetEvWait(0);
	    wait->add_event(wait_event);
	    wait->set_line(*this);

	    delete expr;
	    delete enet;
	    return wait;
      }

	/* Invert the sense of the test with an exclusive NOR. In
	   other words, if this adjusted expression returns TRUE, then
	   wait. */
      assert(expr->expr_width() == 1);
      expr = new NetEBComp('N', expr, new NetEConst(verinum(verinum::V1)));
      NetExpr*tmp = expr->eval_tree();
      if (tmp) {
	    delete expr;
	    expr = tmp;
      }

      NetEvent*wait_event = new NetEvent(scope->local_symbol());
      scope->add_event(wait_event);

      NetEvWait*wait = new NetEvWait(0 /* noop */);
      wait->add_event(wait_event);
      wait->set_line(*this);

      NexusSet*wait_set = expr->nex_input();
      if (wait_set == 0) {
	    cerr << get_line() << ": internal error: No NexusSet"
		 << " from wait expression." << endl;
	    des->errors += 1;
	    return 0;
      }

      if (wait_set->count() == 0) {
	    cerr << get_line() << ": internal error: Empty NexusSet"
		 << " from wait expression." << endl;
	    des->errors += 1;
	    return 0;
      }

      NetEvProbe*wait_pr = new NetEvProbe(scope, scope->local_symbol(),
					  wait_event, NetEvProbe::ANYEDGE,
					  wait_set->count());
      for (unsigned idx = 0; idx < wait_set->count() ;  idx += 1)
	    connect(wait_set[0][idx], wait_pr->pin(idx));

      delete wait_set;
      des->add_node(wait_pr);

      NetWhile*loop = new NetWhile(expr, wait);
      loop->set_line(*this);

	/* If there is no real substatement (i.e., "wait (foo) ;") then
	   we are done. */
      if (enet == 0)
	    return loop;

	/* Create a sequential block to combine the wait loop and the
	   delayed statement. */
      NetBlock*block = new NetBlock(NetBlock::SEQU, 0);
      block->append(loop);
      block->append(enet);
      block->set_line(*this);

      return block;
}


NetProc* PEventStatement::elaborate(Design*des, NetScope*scope) const
{
      NetProc*enet = 0;
      if (statement_) {
	    enet = statement_->elaborate(des, scope);
	    if (enet == 0)
		  return 0;

      } else {
	    enet = new NetBlock(NetBlock::SEQU, 0);
	    enet->set_line(*this);
      }

      if ((expr_.count() == 1) && (expr_[0]->type() == PEEvent::POSITIVE))
	    return elaborate_wait(des, scope, enet);

      return elaborate_st(des, scope, enet);
}

/*
 * Forever statements are represented directly in the netlist. It is
 * theoretically possible to use a while structure with a constant
 * expression to represent the loop, but why complicate the code
 * generators so?
 */
NetProc* PForever::elaborate(Design*des, NetScope*scope) const
{
      NetProc*stat = statement_->elaborate(des, scope);
      if (stat == 0) return 0;

      NetForever*proc = new NetForever(stat);
      return proc;
}

/*
 * Force is like a procedural assignment, most notably prodedural
 * continuous assignment:
 *
 *    force <lval> = <rval>
 *
 * The <lval> can be anything that a normal behavioral assignment can
 * take, plus net signals. This is a little bit more lax then the
 * other proceedural assignments.
 */
NetForce* PForce::elaborate(Design*des, NetScope*scope) const
{
      NetForce*dev = 0;
      assert(scope);

      NetAssign_*lval = lval_->elaborate_lval(des, scope, true);
      if (lval == 0)
	    return 0;

      unsigned lwid = count_lval_width(lval);

      NetExpr*rexp = elab_and_eval(des, scope, expr_, lwid);
      if (rexp == 0)
	    return 0;

      rexp->set_width(lwid, true);
      rexp = pad_to_width(rexp, lwid);

      dev = new NetForce(lval, rexp);

      if (debug_elaborate) {
	    cerr << get_line() << ": debug: ELaborate force,"
		 << " lval width=" << lval->lwidth()
		 << " rval width=" << rexp->expr_width()
		 << " rval=" << *rexp
		 << endl;
      }

      dev->set_line(*this);
      return dev;
}

/*
 * elaborate the for loop as the equivalent while loop. This eases the
 * task for the target code generator. The structure is:
 *
 *     begin : top
 *       name1_ = expr1_;
 *       while (cond_) begin : body
 *          statement_;
 *          name2_ = expr2_;
 *       end
 *     end
 */
NetProc* PForStatement::elaborate(Design*des, NetScope*scope) const
{
      NetExpr*etmp;
      assert(scope);

      const PEIdent*id1 = dynamic_cast<const PEIdent*>(name1_);
      assert(id1);
      const PEIdent*id2 = dynamic_cast<const PEIdent*>(name2_);
      assert(id2);

      NetBlock*top = new NetBlock(NetBlock::SEQU, 0);
      top->set_line(*this);

	/* make the expression, and later the initial assignment to
	   the condition variable. The statement in the for loop is
	   very specifically an assignment. */
      NetNet*sig = des->find_signal(scope, id1->path());
      if (sig == 0) {
	    cerr << id1->get_line() << ": register ``" << id1->path()
		 << "'' unknown in this context." << endl;
	    des->errors += 1;
	    return 0;
      }
      assert(sig);
      NetAssign_*lv = new NetAssign_(sig);

	/* Calculate the width of the initialization as if this were
	   any other assignment statement. */
      unsigned use_width = lv->lwidth();
      bool unsized_flag = false;
      use_width = expr1_->test_width(des, scope, use_width, use_width, unsized_flag);

	/* Make the r-value of the initial assignment, and size it
	   properly. Then use it to build the assignment statement. */
      etmp = elab_and_eval(des, scope, expr1_, use_width);
      etmp->set_width(use_width);
      etmp = pad_to_width(etmp, use_width);

      if (debug_elaborate) {
	    cerr << get_line() << ": debug: FOR initial assign: "
		 << sig->name() << " = " << *etmp << endl;
	    assert(etmp->expr_width() >= lv->lwidth());
      }

      NetAssign*init = new NetAssign(lv, etmp);
      init->set_line(*this);

      top->append(init);

      NetBlock*body = new NetBlock(NetBlock::SEQU, 0);
      body->set_line(*this);

	/* Elaborate the statement that is contained in the for
	   loop. If there is an error, this will return 0 and I should
	   skip the append. No need to worry, the error has been
	   reported so it's OK that the netlist is bogus. */
      NetProc*tmp = statement_->elaborate(des, scope);
      if (tmp)
	    body->append(tmp);


	/* Elaborate the increment assignment statement at the end of
	   the for loop. This is also a very specific assignment
	   statement. Put this into the "body" block. */
      sig = des->find_signal(scope, id2->path());
      if (sig == 0) {
	    cerr << get_line() << ": error: Unable to find variable "
		 << id2->path() << " in for-loop increment expressin." << endl;
	    des->errors += 1;
	    return body;
      }

      assert(sig);
      lv = new NetAssign_(sig);

	/* Make the rvalue of the increment expression, and size it
	   for the lvalue. */
      etmp = expr2_->elaborate_expr(des, scope, lv->lwidth(), false);
      etmp->set_width(lv->lwidth());
      NetAssign*step = new NetAssign(lv, etmp);
      step->set_line(*this);

      body->append(step);


	/* Elaborate the condition expression. Try to evaluate it too,
	   in case it is a constant. This is an interesting case
	   worthy of a warning. */
      NetExpr*ce = elab_and_eval(des, scope, cond_, -1);
      if (ce == 0) {
	    delete top;
	    return 0;
      }

      if (dynamic_cast<NetEConst*>(ce)) {
	    cerr << get_line() << ": warning: condition expression "
		  "of for-loop is constant." << endl;
      }


	/* All done, build up the loop. */

      NetWhile*loop = new NetWhile(ce, body);
      loop->set_line(*this);
      top->append(loop);
      return top;
}

/*
 * (See the PTask::elaborate methods for basic common stuff.)
 *
 * The return value of a function is represented as a reg variable
 * within the scope of the function that has the name of the
 * function. So for example with the function:
 *
 *    function [7:0] incr;
 *      input [7:0] in1;
 *      incr = in1 + 1;
 *    endfunction
 *
 * The scope of the function is <parent>.incr and there is a reg
 * variable <parent>.incr.incr. The elaborate_1 method is called with
 * the scope of the function, so the return reg is easily located.
 *
 * The function parameters are all inputs, except for the synthetic
 * output parameter that is the return value. The return value goes
 * into port 0, and the parameters are all the remaining ports.
 */

void PFunction::elaborate(Design*des, NetScope*scope) const
{
      NetFuncDef*def = scope->func_def();
      if (def == 0) {
	    cerr << get_line() << ": internal error: "
		 << "No function definition for function "
		 << scope->name() << endl;
	    return;
      }

      assert(def);

      NetProc*st = statement_->elaborate(des, scope);
      if (st == 0) {
	    cerr << statement_->get_line() << ": error: Unable to elaborate "
		  "statement in function " << def->name() << "." << endl;
	    des->errors += 1;
	    return;
      }

      def->set_proc(st);
}

NetProc* PRelease::elaborate(Design*des, NetScope*scope) const
{
      assert(scope);

      NetAssign_*lval = lval_->elaborate_lval(des, scope, true);
      if (lval == 0)
	    return 0;

      NetRelease*dev = new NetRelease(lval);
      dev->set_line( *this );
      return dev;
}

NetProc* PRepeat::elaborate(Design*des, NetScope*scope) const
{
      assert(scope);

      NetExpr*expr = elab_and_eval(des, scope, expr_, -1);
      if (expr == 0) {
	    cerr << get_line() << ": Unable to elaborate"
		  " repeat expression." << endl;
	    des->errors += 1;
	    return 0;
      }

      NetProc*stat = statement_->elaborate(des, scope);
      if (stat == 0) return 0;

	// If the expression is a constant, handle certain special
	// iteration counts.
      if (NetEConst*ce = dynamic_cast<NetEConst*>(expr)) {
	    verinum val = ce->value();
	    switch (val.as_ulong()) {
		case 0:
		  delete expr;
		  delete stat;
		  return new NetBlock(NetBlock::SEQU, 0);
		case 1:
		  delete expr;
		  return stat;
		default:
		  break;
	    }
      }

      NetRepeat*proc = new NetRepeat(expr, stat);
      return proc;
}

/*
 * A task definition is elaborated by elaborating the statement that
 * it contains, and connecting its ports to NetNet objects. The
 * netlist doesn't really need the array of parameters once elaboration
 * is complete, but this is the best place to store them.
 *
 * The first elaboration pass finds the reg objects that match the
 * port names, and creates the NetTaskDef object. The port names are
 * in the form task.port.
 *
 *      task foo;
 *        output blah;
 *        begin <body> end
 *      endtask
 *
 * So in the foo example, the PWire objects that represent the ports
 * of the task will include a foo.blah for the blah port. This port is
 * bound to a NetNet object by looking up the name. All of this is
 * handled by the PTask::elaborate_sig method and the results stashed
 * in the created NetTaskDef attached to the scope.
 *
 * Elaboration pass 2 for the task definition causes the statement of
 * the task to be elaborated and attached to the NetTaskDef object
 * created in pass 1.
 *
 * NOTE: I am not sure why I bothered to prepend the task name to the
 * port name when making the port list. It is not really useful, but
 * that is what I did in pform_make_task_ports, so there it is.
 */

void PTask::elaborate(Design*des, NetScope*task) const
{
      NetTaskDef*def = task->task_def();
      assert(def);

      NetProc*st;
      if (statement_ == 0) {
	    st = new NetBlock(NetBlock::SEQU, 0);

      } else {

	    st = statement_->elaborate(des, task);
	    if (st == 0) {
		  cerr << statement_->get_line() << ": Unable to elaborate "
			"statement in task " << task->name()
		       << " at " << get_line() << "." << endl;
		  return;
	    }
      }

      def->set_proc(st);
}

NetProc* PTrigger::elaborate(Design*des, NetScope*scope) const
{
      assert(scope);

      NetNet*       sig = 0;
      NetMemory*    mem = 0;
      const NetExpr*par = 0;
      NetEvent*     eve = 0;

      NetScope*found_in = symbol_search(des, scope, event_,
					sig, mem, par, eve);

      if (found_in == 0) {
	    cerr << get_line() << ": error: event <" << event_ << ">"
		 << " not found." << endl;
	    des->errors += 1;
	    return 0;
      }

      if (eve == 0) {
	    cerr << get_line() << ": error:  <" << event_ << ">"
		 << " is not a named event." << endl;
	    des->errors += 1;
	    return 0;
      }

      NetEvTrig*trig = new NetEvTrig(eve);
      trig->set_line(*this);
      return trig;
}

/*
 * The while loop is fairly directly represented in the netlist.
 */
NetProc* PWhile::elaborate(Design*des, NetScope*scope) const
{
      NetWhile*loop = new NetWhile(elab_and_eval(des, scope, cond_, -1),
				   statement_->elaborate(des, scope));
      return loop;
}

void PSpecPath::elaborate(Design*des, NetScope*scope) const
{
      uint64_t delay_value[12];
      unsigned ndelays = 0;

	/* Do not elaborate specify delay paths if this feature is
	   turned off. */
      if (!gn_specify_blocks_flag)
	    return;

      ndelays = delays.size();
      if (ndelays > 12)
	    ndelays = 12;

      int shift = scope->time_unit() - des->get_precision();

	/* Elaborate the delay values themselves. Remember to scale
	   them for the timescale/precision of the scope. */
      for (unsigned idx = 0 ;  idx < ndelays ;  idx += 1) {
	    PExpr*exp = delays[idx];
	    NetExpr*cur = elab_and_eval(des, scope, exp, 0);

	    if (NetEConst*cur_con = dynamic_cast<NetEConst*> (cur)) {
		  delay_value[idx] = cur_con->value().as_ulong();
		  for (int tmp = 0 ;  tmp < shift ;  tmp += 1)
			delay_value[idx] *= 10;

	    } else if (NetECReal*cur_rcon = dynamic_cast<NetECReal*>(cur)) {
		  delay_value[idx] = cur_rcon->value().as_long(shift);

	    } else {
		  cerr << get_line() << ": error: Path delay value "
		       << "must be constant." << endl;
		  delay_value[idx] = 0;
		  des->errors += 1;
	    }
	    delete cur;
      }

      switch (ndelays) {
	  case 1:
	  case 2:
	  case 3:
	  case 6:
	  case 12:
	    break;
	  default:
	    cerr << get_line() << ": error: Incorrect delay configuration."
		 << endl;
	    ndelays = 1;
	    des->errors += 1;
	    break;
      }

	/* Create all the various paths from the path specifier. */
      typedef std::vector<perm_string>::const_iterator str_vector_iter;
      for (str_vector_iter cur = dst.begin()
		 ; cur != dst.end() ;  cur ++) {

	    if (debug_elaborate) {
		  cerr << get_line() << ": debug: Path to " << (*cur) << endl;
	    }

	    NetNet*dst_sig = scope->find_signal(*cur);
	    if (dst_sig == 0) {
		  cerr << get_line() << ": error: No such wire "
		       << *cur << " in this module." << endl;
		  des->errors += 1;
		  continue;
	    }

	    NetDelaySrc*path = new NetDelaySrc(scope, scope->local_symbol(),
					       src.size());
	    path->set_line(*this);

	    switch (ndelays) {
		case 12:
		  path->set_delays(delay_value[0],  delay_value[1],
				   delay_value[2],  delay_value[3],
				   delay_value[4],  delay_value[5],
				   delay_value[6],  delay_value[7],
				   delay_value[8],  delay_value[9],
				   delay_value[10], delay_value[11]);
		  break;
		case 6:
		  path->set_delays(delay_value[0], delay_value[1],
				   delay_value[2], delay_value[3],
				   delay_value[4], delay_value[5]);
		  break;
		case 3:
		  path->set_delays(delay_value[0], delay_value[1],
				   delay_value[2]);
		  break;
		case 2:
		  path->set_delays(delay_value[0], delay_value[1]);
		  break;
		case 1:
		  path->set_delays(delay_value[0]);
		  break;
	    }

	    unsigned idx = 0;
	    for (str_vector_iter cur_src = src.begin()
		       ; cur_src != src.end() ;  cur_src ++) {
		  NetNet*src_sig = scope->find_signal(*cur_src);
		  assert(src_sig);

		  connect(src_sig->pin(0), path->pin(idx));
		  idx += 1;
	    }

	    dst_sig->add_delay_path(path);
      }

}

/*
 * When a module is instantiated, it creates the scope then uses this
 * method to elaborate the contents of the module.
 */
bool Module::elaborate(Design*des, NetScope*scope) const
{
      bool result_flag = true;

      if (gn_specify_blocks_flag) {
	      // Elaborate specparams
	    typedef map<perm_string,PExpr*>::const_iterator specparam_it_t;
	    for (specparam_it_t cur = specparams.begin()
		       ; cur != specparams.end() ; cur ++ ) {

		  NetExpr*val = elab_and_eval(des, scope, (*cur).second, -1);
		  NetScope::spec_val_t value;

		  if (NetECReal*val_c = dynamic_cast<NetECReal*> (val)) {

			value.type     = IVL_VT_REAL;
			value.real_val = val_c->value().as_double();

			if (debug_elaborate)
			      cerr << get_line() << ": debug: Elaborate "
				   << "specparam " << (*cur).first
				   << " value=" << value.real_val << endl;

		  } else if (NetEConst*val_c = dynamic_cast<NetEConst*> (val)) {

			value.type    = IVL_VT_BOOL;
			value.integer = val_c->value().as_long();

			if (debug_elaborate)
			      cerr << get_line() << ": debug: Elaborate "
				   << "specparam " << (*cur).first
				   << " value=" << value.integer << endl;

		  } else {
			cerr << (*cur).second->get_line() << ": error: "
			     << "specparam " << (*cur).first << " value"
			     << " is not constant: " << *val << endl;
			des->errors += 1;
		  }

		  assert(val);
		  delete  val;
		  scope->specparams[(*cur).first] = value;
	    }
      }

	// Elaborate within the generate blocks.
      typedef list<PGenerate*>::const_iterator generate_it_t;
      for (generate_it_t cur = generate_schemes.begin()
		 ; cur != generate_schemes.end() ; cur ++ ) {
	    (*cur)->elaborate(des);
      }

	// Elaborate functions.
      typedef map<perm_string,PFunction*>::const_iterator mfunc_it_t;
      for (mfunc_it_t cur = funcs_.begin()
		 ; cur != funcs_.end() ;  cur ++) {

	    NetScope*fscope = scope->child((*cur).first);
	    assert(fscope);
	    (*cur).second->elaborate(des, fscope);
      }

	// Elaborate the task definitions. This is done before the
	// behaviors so that task calls may reference these, and after
	// the signals so that the tasks can reference them.
      typedef map<perm_string,PTask*>::const_iterator mtask_it_t;
      for (mtask_it_t cur = tasks_.begin()
		 ; cur != tasks_.end() ;  cur ++) {

	    NetScope*tscope = scope->child((*cur).first);
	    assert(tscope);
	    (*cur).second->elaborate(des, tscope);
      }

	// Get all the gates of the module and elaborate them by
	// connecting them to the signals. The gate may be simple or
	// complex.
      const list<PGate*>&gl = get_gates();

      for (list<PGate*>::const_iterator gt = gl.begin()
		 ; gt != gl.end()
		 ; gt ++ ) {

	    (*gt)->elaborate(des, scope);
      }

	// Elaborate the behaviors, making processes out of them. This
	// involves scanning the PProcess* list, creating a NetProcTop
	// for each process.
      const list<PProcess*>&sl = get_behaviors();

      for (list<PProcess*>::const_iterator st = sl.begin()
		 ; st != sl.end()
		 ; st ++ ) {

	    NetProc*cur = (*st)->statement()->elaborate(des, scope);
	    if (cur == 0) {
		  result_flag = false;
		  continue;
	    }

	    NetProcTop*top=NULL;
	    switch ((*st)->type()) {
		case PProcess::PR_INITIAL:
		  top = new NetProcTop(scope, NetProcTop::KINITIAL, cur);
		  break;
		case PProcess::PR_ALWAYS:
		  top = new NetProcTop(scope, NetProcTop::KALWAYS, cur);
		  break;
	    }
	    assert(top);

	      // Evaluate the attributes for this process, if there
	      // are any. These attributes are to be attached to the
	      // NetProcTop object.
	    struct attrib_list_t*attrib_list = 0;
	    unsigned attrib_list_n = 0;
	    attrib_list = evaluate_attributes((*st)->attributes,
					      attrib_list_n,
					      des, scope);

	    for (unsigned adx = 0 ;  adx < attrib_list_n ;  adx += 1)
		  top->attribute(attrib_list[adx].key,
				 attrib_list[adx].val);

	    delete[]attrib_list;

	    top->set_line(*(*st));
	    des->add_process(top);

	      /* Detect the special case that this is a combinational
		 always block. We want to attach an _ivl_schedule_push
		 attribute to this process so that it starts up and
		 gets into its wait statement before non-combinational
		 code is executed. */
	    do {
		  if (top->type() != NetProcTop::KALWAYS)
			break;

		  NetEvWait*st = dynamic_cast<NetEvWait*>(top->statement());
		  if (st == 0)
			break;

		  if (st->nevents() != 1)
			break;

		  NetEvent*ev = st->event(0);

		  if (ev->nprobe() == 0)
			break;

		  bool anyedge_test = true;
		  for (unsigned idx = 0 ;  anyedge_test && (idx<ev->nprobe())
			     ; idx += 1) {
			const NetEvProbe*pr = ev->probe(idx);
			if (pr->edge() != NetEvProbe::ANYEDGE)
			      anyedge_test = false;
		  }

		  if (! anyedge_test)
			break;

		  top->attribute(perm_string::literal("_ivl_schedule_push"),
				 verinum(1));
	    } while (0);

      }

	// Elaborate the specify paths of the module.

      for (list<PSpecPath*>::const_iterator sp = specify_paths.begin()
		 ; sp != specify_paths.end() ;  sp ++) {

	    (*sp)->elaborate(des, scope);
      }

      return result_flag;
}

bool PGenerate::elaborate(Design*des) const
{
      bool flag = true;

      typedef list<NetScope*>::const_iterator scope_list_it_t;
      for (scope_list_it_t cur = scope_list_.begin()
		 ; cur != scope_list_.end() ; cur ++ ) {

	    if (debug_elaborate)
		  cerr << get_line() << ": debug: Elaborate in "
		       << "scope " << (*cur)->name() << endl;

	    flag = elaborate_(des, *cur) & flag;
      }

      return flag;
}

bool PGenerate::elaborate_(Design*des, NetScope*scope) const
{
      typedef list<PGate*>::const_iterator gates_it_t;
      for (gates_it_t cur = gates.begin() ; cur != gates.end() ; cur ++ ) {

	    (*cur)->elaborate(des, scope);
      }

      return true;
}

struct root_elem {
      Module *mod;
      NetScope *scope;
};

Design* elaborate(list<perm_string>roots)
{
      svector<root_elem*> root_elems(roots.size());
      bool rc = true;
      unsigned i = 0;

	// This is the output design. I fill it in as I scan the root
	// module and elaborate what I find.
      Design*des = new Design;

	// Scan the root modules, and elaborate their scopes.
      for (list<perm_string>::const_iterator root = roots.begin()
		 ; root != roots.end()
		 ; root++) {

	      // Look for the root module in the list.
	    map<perm_string,Module*>::const_iterator mod = pform_modules.find(*root);
	    if (mod == pform_modules.end()) {
		  cerr << "error: Unable to find the root module \""
		       << (*root) << "\" in the Verilog source." << endl;
		  cerr << "     : Perhaps ``-s " << (*root)
		       << "'' is incorrect?" << endl;
		  des->errors++;
		  continue;
	    }

	      // Get the module definition for this root instance.
	    Module *rmod = (*mod).second;

	      // Make the root scope.
	    NetScope*scope = des->make_root_scope(*root);
	    scope->time_unit(rmod->time_unit);
	    scope->time_precision(rmod->time_precision);
	    scope->default_nettype(rmod->default_nettype);
	    des->set_precision(rmod->time_precision);

	    Module::replace_t stub;

	      // Recursively elaborate from this root scope down. This
	      // does a lot of the grunt work of creating sub-scopes, etc.
	    if (! rmod->elaborate_scope(des, scope, stub)) {
		  delete des;
		  return 0;
	    }

	    struct root_elem *r = new struct root_elem;
	    r->mod = rmod;
	    r->scope = scope;
	    root_elems[i++] = r;
      }

	// Errors already? Probably missing root modules. Just give up
	// now and return nothing.
      if (des->errors > 0)
	    return des;

	// This method recurses through the scopes, looking for
	// defparam assignments to apply to the parameters in the
	// various scopes. This needs to be done after all the scopes
	// and basic parameters are taken care of because the defparam
	// can assign to a parameter declared *after* it.
      des->run_defparams();


	// At this point, all parameter overrides are done. Scan the
	// scopes and evaluate the parameters all the way down to
	// constants.
      des->evaluate_parameters();

	// With the parameters evaluated down to constants, we have
	// what we need to elaborate signals and memories. This pass
	// creates all the NetNet and NetMemory objects for declared
	// objects.
      for (i = 0; i < root_elems.count(); i++) {
	    Module *rmod = root_elems[i]->mod;
	    NetScope *scope = root_elems[i]->scope;

	    if (! rmod->elaborate_sig(des, scope)) {
		  delete des;
		  return 0;
	    }
      }

	// Now that the structure and parameters are taken care of,
	// run through the pform again and generate the full netlist.
      for (i = 0; i < root_elems.count(); i++) {
	    Module *rmod = root_elems[i]->mod;
	    NetScope *scope = root_elems[i]->scope;

	    rc &= rmod->elaborate(des, scope);
      }


      if (rc == false) {
	    delete des;
	    des = 0;
      }

      return des;
}


/*
 * $Log: elaborate.cc,v $
 * Revision 1.353  2006/12/08 04:09:41  steve
 *  @* without inputs is an error.
 *
 * Revision 1.352  2006/11/27 02:01:07  steve
 *  Fix crash handling constant true conditional.
 *
 * Revision 1.351  2006/11/26 07:10:30  steve
 *  Fix compile time eval of condition expresion to do reduction OR of vectors.
 *
 * Revision 1.350  2006/11/26 06:29:16  steve
 *  Fix nexus widths for direct link assign and ternary nets.
 *
 * Revision 1.349  2006/11/04 06:19:25  steve
 *  Remove last bits of relax_width methods, and use test_width
 *  to calculate the width of an r-value expression that may
 *  contain unsized numbers.
 *
 * Revision 1.348  2006/10/30 05:44:49  steve
 *  Expression widths with unsized literals are pseudo-infinite width.
 *
 * Revision 1.347  2006/10/03 15:33:49  steve
 *  no-specify turns of specparam elaboration.
 *
 * Revision 1.346  2006/10/03 05:06:00  steve
 *  Support real valued specify delays, properly scaled.
 *
 * Revision 1.345  2006/09/28 04:35:18  steve
 *  Support selective control of specify and xtypes features.
 *
 * Revision 1.344  2006/09/26 19:48:40  steve
 *  Missing PSpec.cc file.
 *
 * Revision 1.343  2006/09/23 04:57:19  steve
 *  Basic support for specify timing.
 *
 * Revision 1.342  2006/09/22 22:14:27  steve
 *  Proper error message when logic array pi count is bad.
 *
 * Revision 1.341  2006/08/08 05:11:37  steve
 *  Handle 64bit delay constants.
 *
 * Revision 1.340  2006/06/02 04:48:50  steve
 *  Make elaborate_expr methods aware of the width that the context
 *  requires of it. In the process, fix sizing of the width of unary
 *  minus is context determined sizes.
 *
 * Revision 1.339  2006/05/01 20:47:59  steve
 *  More explicit datatype setup.
 *
 * Revision 1.338  2006/04/30 05:17:48  steve
 *  Get the data type of part select results right.
 *
 * Revision 1.337  2006/04/26 04:43:50  steve
 *  Chop down assign r-values that elaborate too wide.
 *
 * Revision 1.336  2006/04/10 00:37:42  steve
 *  Add support for generate loops w/ wires and gates.
 *
 * Revision 1.335  2006/03/30 01:49:07  steve
 *  Fix instance arrays indexed by overridden parameters.
 *
 * Revision 1.334  2006/01/03 05:22:14  steve
 *  Handle complex net node delays.
 *
 * Revision 1.333  2006/01/02 05:33:19  steve
 *  Node delays can be more general expressions in structural contexts.
 *
 * Revision 1.332  2005/11/26 00:35:42  steve
 *  More precise about r-value width of constants.
 *
 * Revision 1.331  2005/11/10 13:28:11  steve
 *  Reorganize signal part select handling, and add support for
 *  indexed part selects.
 *
 *  Expand expression constant propagation to eliminate extra
 *  sums in certain cases.
 *
 * Revision 1.330  2005/09/27 04:51:37  steve
 *  Error message for invalid for-loop index variable.
 *
 * Revision 1.329  2005/09/14 02:53:13  steve
 *  Support bool expressions and compares handle them optimally.
 *
 * Revision 1.328  2005/08/06 17:58:16  steve
 *  Implement bi-directional part selects.
 *
 * Revision 1.327  2005/07/15 00:41:09  steve
 *  More debug information.
 *
 * Revision 1.326  2005/07/11 16:56:50  steve
 *  Remove NetVariable and ivl_variable_t structures.
 *
 * Revision 1.325  2005/06/17 05:06:47  steve
 *  Debug messages.
 *
 * Revision 1.324  2005/05/24 01:44:27  steve
 *  Do sign extension of structuran nets.
 *
 * Revision 1.323  2005/05/17 20:56:55  steve
 *  Parameters cannot have their width changed.
 *
 * Revision 1.322  2005/05/13 05:12:39  steve
 *  Some debug messages.
 *
 * Revision 1.321  2005/04/24 23:44:01  steve
 *  Update DFF support to new data flow.
 *
 * Revision 1.320  2005/03/05 05:38:33  steve
 *  Get rval width right for arguments into task calls.
 *
 * Revision 1.319  2005/02/19 02:43:38  steve
 *  Support shifts and divide.
 *
 * Revision 1.318  2005/02/10 04:56:58  steve
 *  distinguish between single port namy instances, and single instances many sub-ports.
 *
 * Revision 1.317  2005/02/08 00:12:36  steve
 *  Add the NetRepeat node, and code generator support.
 *
 * Revision 1.316  2005/01/30 01:42:05  steve
 *  Debug messages for PGAssign elaboration.
 *
 * Revision 1.315  2005/01/22 18:16:00  steve
 *  Remove obsolete NetSubnet class.
 *
 * Revision 1.314  2005/01/12 03:17:36  steve
 *  Properly pad vector widths in pgassign.
 *
 * Revision 1.313  2005/01/09 20:16:01  steve
 *  Use PartSelect/PV and VP to handle part selects through ports.
 *
 * Revision 1.312  2004/12/29 23:55:43  steve
 *  Unify elaboration of l-values for all proceedural assignments,
 *  including assing, cassign and force.
 *
 *  Generate NetConcat devices for gate outputs that feed into a
 *  vector results. Use this to hande gate arrays. Also let gate
 *  arrays handle vectors of gates when the outputs allow for it.
 *
 * Revision 1.311  2004/12/15 17:09:11  steve
 *  Force r-value padded to width.
 *
 * Revision 1.310  2004/12/12 18:13:39  steve
 *  Fix r-value width of continuous assign.
 *
 * Revision 1.309  2004/12/11 02:31:25  steve
 *  Rework of internals to carry vectors through nexus instead
 *  of single bits. Make the ivl, tgt-vvp and vvp initial changes
 *  down this path.
 *
 */

