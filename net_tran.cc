/*
 * Copyright (c) 2008 Stephen Williams (steve@icarus.com)
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

# include "config.h"

# include <iostream>

# include  <typeinfo>
# include  <cstdlib>
# include  "compiler.h"
# include  "netlist.h"
# include  "netmisc.h"
# include  "ivl_target_priv.h"
# include  "ivl_assert.h"

static bool has_enable(ivl_switch_type_t tt)
{
      switch (tt) {
	  case IVL_SW_TRANIF0:
	  case IVL_SW_TRANIF1:
	  case IVL_SW_RTRANIF0:
	  case IVL_SW_RTRANIF1:
	    return true;
	  default:
	    return false;
      }
}

NetTran::NetTran(NetScope*scope, perm_string n, ivl_switch_type_t tt)
: NetNode(scope, n, has_enable(tt)? 3 : 2), type_(tt)
{
      pin(0).set_dir(Link::PASSIVE); pin(0).set_name(perm_string::literal("A"), 0);
      pin(1).set_dir(Link::PASSIVE); pin(1).set_name(perm_string::literal("B"), 0);
      if (pin_count() == 3) {
	    pin(2).set_dir(Link::INPUT);
	    pin(2).set_name(perm_string::literal("E"), 0);
      }
}

NetTran::NetTran(NetScope*scope, perm_string n, unsigned wid, unsigned part, unsigned off)
: NetNode(scope, n, 2), type_(IVL_SW_TRAN_VP), wid_(wid), part_(part), off_(off)
{
      pin(0).set_dir(Link::PASSIVE); pin(0).set_name(perm_string::literal("A"), 0);
      pin(1).set_dir(Link::PASSIVE); pin(1).set_name(perm_string::literal("B"), 0);
}

NetTran::~NetTran()
{
}

unsigned NetTran::vector_width() const
{
      return wid_;
}

unsigned NetTran::part_width() const
{
      return part_;
}

unsigned NetTran::part_offset() const
{
      return off_;
}

void join_island(NetObj*obj)
{
      IslandBranch*branch = dynamic_cast<IslandBranch*> (obj);

	// If this is not even a branch, then stop now.
      if (branch == 0)
	    return;

	// If this is a branch, but already given to an island, then
	// stop.
      if (branch->island)
	    return;

      list<NetObj*> uncommitted_neighbors;

	// Look for neighboring objects that might already be in
	// islands. If we find something, then join that island.
      for (unsigned idx = 0 ; idx < obj->pin_count() ; idx += 1) {
	    Nexus*nex = obj->pin(idx).nexus();
	    for (Link*cur = nex->first_nlink() ; cur ; cur = cur->next_nlink()) {
		  unsigned pin;
		  NetObj*tmp;
		  cur->cur_link(tmp, pin);

		    // Skip self.
		  if (tmp == obj)
			continue;

		    // If tmb is not a branch, then skip it.
		  IslandBranch*tmp_branch = dynamic_cast<IslandBranch*> (tmp);
		  if (tmp_branch == 0)
			continue;

		    // If that is an uncommitted branch, then save
		    // it. When I finally choose an island for self,
		    // these branches will be scanned so that they join
		    // this island as well.
		  if (tmp_branch->island == 0) {
			uncommitted_neighbors.push_back(tmp);
			continue;
		  }

		  ivl_assert(*obj, branch->island==0 || branch->island==tmp_branch->island);

		    // We found an existing island to join. Join it
		    // now. Keep scanning in order to find more neighbors.
		  if (branch->island == 0) {
			if (debug_elaborate)
			      cerr << obj->get_fileline() << ": debug: "
				   << "Join branch to existing island." << endl;
			branch->island = tmp_branch->island;

		  } else if (branch->island != tmp_branch->island) {
			cerr << obj->get_fileline() << ": internal error: "
			     << "Oops, Found 2 neighboring islands." << endl;
			ivl_assert(*obj, 0);
		  }
	    }
      }

	// If after all that we did not find an island to join, then
	// start the island not and join it.
      if (branch->island == 0) {
	    branch->island = new ivl_island_s;
	    branch->island->discipline = 0;
	    if (debug_elaborate)
		  cerr << obj->get_fileline() << ": debug: "
		       << "Create new island for this branch" << endl;
      }

	// Now scan all the uncommitted neighbors I found. Calling
	// join_island() on them will cause them to notice me in the
	// process, and thus they will join my island. This process
	// will recurse until all the connected branches join this island.
      for (list<NetObj*>::iterator cur = uncommitted_neighbors.begin()
		 ; cur != uncommitted_neighbors.end() ; cur ++ ) {
	    join_island(*cur);
      }
}