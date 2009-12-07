/***************************************************************************
  file : $URL$
  version : $LastChangedRevision$  $LastChangedBy$
  date : $LastChangedDate$
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 * Copyright (C) 2007 by Johan De Taeye                                    *
 *                                                                         *
 * This library is free software; you can redistribute it and/or modify it *
 * under the terms of the GNU Lesser General Public License as published   *
 * by the Free Software Foundation; either version 2.1 of the License, or  *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This library is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser *
 * General Public License for more details.                                *
 *                                                                         *
 * You should have received a copy of the GNU Lesser General Public        *
 * License along with this library; if not, write to the Free Software     *
 * Foundation Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 *
 * USA                                                                     *
 *                                                                         *
 ***************************************************************************/

#define FREPPLE_CORE
#include "frepple/model.h"

namespace frepple
{

DECLARE_EXPORT const MetaClass* OperationPlan::metadata;
DECLARE_EXPORT const MetaCategory* OperationPlan::metacategory;
DECLARE_EXPORT unsigned long OperationPlan::counter = 1;


int OperationPlan::initialize()
{
  // Initialize the metadata
  OperationPlan::metacategory = new MetaCategory("operationplan", "operationplans",
    OperationPlan::createOperationPlan, OperationPlan::writer);
  OperationPlan::metadata = new MetaClass("operationplan", "operationplan");

  // Initialize the Python type
  PythonType& x = FreppleCategory<OperationPlan>::getType();
  x.setName("operationplan");
  x.setDoc("frePPLe operationplan");
  x.supportgetattro();
  x.supportsetattro();
  x.supportcreate(create);
  x.addMethod("toXML", toXML, METH_VARARGS, "return a XML representation");
  const_cast<MetaClass*>(metadata)->pythonClass = x.type_object();
  return x.typeReady();
}


void DECLARE_EXPORT OperationPlan::setChanged(bool b)
{
  if (owner)
    owner->setChanged(b);
  else
  {
    oper->setChanged(b);
    if (dmd) dmd->setChanged();
  }
}


DECLARE_EXPORT Object* OperationPlan::createOperationPlan
(const MetaClass* cat, const AttributeList& in)
{
  // Pick up the action attribute
  Action action = MetaClass::decodeAction(in);

  // Decode the attributes
  const DataElement* opnameElement = in.get(Tags::tag_operation);
  if (!*opnameElement && action!=REMOVE)
    throw DataException("Missing operation attribute");
  string opname = *opnameElement ? opnameElement->getString() : "";

  // Decode the operationplan identifier
  unsigned long id = 0;
  const DataElement* idfier = in.get(Tags::tag_id);
  if (*idfier) id = idfier->getUnsignedLong();

  // If an ID is specified, we look up this operation plan
  OperationPlan* opplan = NULL;
  if (idfier)
  {
    opplan = OperationPlan::findId(id);
    if (opplan && !opname.empty()
        && opplan->getOperation()->getName()==opname)
    {
      // Previous and current operations don't match.
      ostringstream ch;
      ch << "Operationplan id " << id
      << " defined multiple times with different operations: '"
      << opplan->getOperation() << "' & '" << opname << "'";
      throw DataException(ch.str());
    }
  }

  // Execute the proper action
  switch (action)
  {
    case REMOVE:
      if (opplan)
      {
        // Send out the notification to subscribers
        if (opplan->getType().raiseEvent(opplan, SIG_REMOVE))
          // Delete it
          delete opplan;
        else
        {
          // The callbacks disallowed the deletion!
          ostringstream ch;
          ch << "Can't delete operationplan with id " << id;
          throw DataException(ch.str());
        }
      }
      else
      {
        ostringstream ch;
        ch << "Can't find operationplan with identifier "
        << id << " for removal";
        throw DataException(ch.str());
      }
      return NULL;
    case ADD:
      if (opplan)
      {
        ostringstream ch;
        ch << "Operationplan with identifier " << id
        << " already exists and can't be added again";
        throw DataException(ch.str());
      }
      if (opname.empty())
        throw DataException
          ("Operation name missing for creating an operationplan");
      break;
    case CHANGE:
      if (!opplan)
      {
        ostringstream ch;
        ch << "Operationplan with identifier " << id << " doesn't exist";
        throw DataException(ch.str());
      }
      break;
    case ADD_CHANGE: ;
  }

  // Return the existing operationplan
  if (opplan) return opplan;

  // Create a new operation plan
  Operation* oper = Operation::find(opname);
  if (!oper)
  {
    // Can't create operationplan because the operation doesn't exist
    throw DataException("Operation '" + opname + "' doesn't exist");
  }
  else
  {
    // Create an operationplan
    opplan = oper->createOperationPlan(0.0,Date::infinitePast,Date::infinitePast,NULL,NULL,id,false);
    if (!opplan->getType().raiseEvent(opplan, SIG_ADD))
    {
      delete opplan;
      throw DataException("Can't create operationplan");
    }
    return opplan;
  }
}


DECLARE_EXPORT OperationPlan* OperationPlan::findId(unsigned long l)
{
  // We are garantueed that there are no operationplans that have an id equal
  // or higher than the current counter. This is garantueed by the
  // instantiate() method.
  if (l >= counter) return NULL;

  // Loop through all operationplans.
  for (OperationPlan::iterator i = begin(); i != end(); ++i)
    if (i->id == l) return &*i;

  // This ID was not found
  return NULL;
}


DECLARE_EXPORT bool OperationPlan::instantiate()
{
  // At least a valid operation pointer must exist
  if (!oper) throw LogicException("Initializing an invalid operationplan");

  // Avoid zero quantity on top-operationplans
  if (getQuantity() <= 0.0 && !owner)
  {
    delete this;
    return false;
  }

  // See if we can consolidate this operationplan with an existing one.
  // Merging is possible only when all the following conditions are met:
  //   - id of the new opplan is not set
  //   - id of the old opplan is set
  //   - it is a fixedtime operation
  //   - it doesn't load any resources
  //   - both operationplans aren't locked
  //   - both operationplans have no owner
  //   - start and end date of both operationplans are the same
  //   - demand of both operationplans are the same
  //   - maximum operation size is not exceeded
  //   - @todo need to check that all flowplans are on the same alternate!!!
  if (!id && getOperation()->getType() == *OperationFixedTime::metadata
    && !getLocked() && !getOwner() && getOperation()->getLoads().empty())
  {
    // Loop through candidates
    OperationPlan *x = oper->last_opplan;
    OperationPlan *y = NULL;
    while (x && !(*x < *this))
    {
      y = x;
      x = x->prev;
    }
    if (y && y->getDates() == getDates() && !y->getOwner()
      && y->getDemand() == getDemand() && !y->getLocked() && y->id
      && y->getQuantity() + getQuantity() < getOperation()->getSizeMaximum())
    {
      // Merging with the 'next' operationplan
      y->setQuantity(y->getQuantity() + getQuantity());
      delete this;
      return false;
    }
    if (x && x->getDates() == getDates() && !x->getOwner()
      && x->getDemand() == getDemand() && !x->getLocked() && x->id
      && x->getQuantity() + getQuantity() < getOperation()->getSizeMaximum())
    {
      // Merging with the 'previous' operationplan
      x->setQuantity(x->getQuantity() + getQuantity());
      delete this;
      return false;
    }
  }

  // Instantiate all suboperationplans as well
  for (OperationPlan *x = firstsubopplan; x; x = x->nextsubopplan)
  {
    logger << " Instant " << x->getOperation() << endl;
    x->instantiate();
  }

  // Create unique identifier
  // Having an identifier assigned is an important flag.
  // Only operation plans with an id :
  //   - can be linked in the global operation plan list.
  //   - can have problems (this results from the previous point).
  //   - can be linked with a demand.
  // These properties allow us to delete operation plans without an id faster.
  static Mutex onlyOne;
  {
  ScopeMutexLock l(onlyOne);  // Need to assure that ids are unique!
  if (id)
  {
    // An identifier was read in from input
    if (id < counter)
    {
      // The assigned id potentially clashes with an existing operationplan.
      // Check whether it clashes with existing operationplans
      OperationPlan* opplan = findId(id);
      if (opplan && opplan->getOperation()!=oper)
      {
        ostringstream ch;
        ch << "Operationplan id " << id
          << " defined multiple times with different operations: '"
          << opplan->getOperation() << "' & '" << oper << "'";
        delete this;
        throw DataException(ch.str());
      }
    }
    else
      // The new operationplan definately doesn't clash with existing id's.
      // The counter need updating to garantuee that counter is always
      // a safe starting point for tagging new operationplans.
      counter = id+1;
  }
  else
    // Fresh operationplan with blank id
    id = counter++;
  }

  // Insert into the doubly linked list of operationplans.
  insertInOperationplanList();

  // If we used the lazy creator, the flow- and loadplans have not been
  // created yet. We do it now...
  createFlowLoads();

  // Extra registration step if this is a delivery operation
  if (getDemand() && getDemand()->getDeliveryOperation() == oper)
    dmd->addDelivery(this);

  // Mark the operation to detect its problems
  // Note that a single operationplan thus retriggers the problem computation
  // for all operationplans of this operation. For models with 1) a large
  // number of operationplans per operation and 2) very frequent problem
  // detection, this could constitute a scalability problem. This combination
  // is expected to be unusual and rare, justifying this design choice.
  oper->setChanged();

  // The operationplan is valid
  return true;
}


DECLARE_EXPORT void OperationPlan::insertInOperationplanList()
{

  // Check if already linked
  if (prev || oper->first_opplan == this) return;

  if (!oper->first_opplan)
  {
    // First operationplan in the list
    oper->first_opplan = this;
    oper->last_opplan = this;
  }
  else if (*this < *(oper->first_opplan))
  {
    // First in the list
    next = oper->first_opplan;
    next->prev = this;
    oper->first_opplan = this;
  }
  else if (*(oper->last_opplan) < *this)
  {
    // Last in the list
    prev = oper->last_opplan;
    prev->next = this;
    oper->last_opplan = this;
  }
  else
  {
    // Insert in the middle of the list
    OperationPlan *x = oper->last_opplan;
    OperationPlan *y = NULL;
    while (!(*x < *this))
    {
      y = x;
      x = x->prev;
    }
    next = y;
    prev = x;
    if (x) x->next = this;
    if (y) y->prev = this;
  }
}


DECLARE_EXPORT void OperationPlan::addSubOperationPlan(OperationPlan* o)
{
  // Check
  if (!o) throw LogicException("Adding null suboperationplan");

  // Adding a suboperationplan that was already added
  if (o->owner == this)  return;

  // Clear the previous owner, if there is one
  if (o->owner) o->owner->eraseSubOperationPlan(o);

  // Link in the list, keeping the right ordering
  if (!firstsubopplan)
  {
    // First element
    firstsubopplan = o;
    lastsubopplan = o;
  }
  else if (*o < *firstsubopplan)
  {
    // New head
    o->nextsubopplan = firstsubopplan;
    firstsubopplan->prevsubopplan = o;
    firstsubopplan = o;
  }
  else
  {
    // Insert in the middle or at the end
    OperationPlan *x = firstsubopplan;
    while (x->nextsubopplan && *(x->nextsubopplan) < *o) x = x->nextsubopplan;
    o->nextsubopplan = x->nextsubopplan;
    o->prevsubopplan = x;
    if (x->nextsubopplan)
      x->nextsubopplan->prevsubopplan = o;
    else
      lastsubopplan = o; // New tail
    x->nextsubopplan = o;
  }
  o->owner = this;

  // Update the top operationplan
  setStartAndEnd(
    firstsubopplan->getDates().getStart(),
    lastsubopplan->getDates().getEnd()
  );

  // Update the flow and loadplans
  update();
}


DECLARE_EXPORT void OperationPlan::eraseSubOperationPlan(OperationPlan* o)
{
  // Check
  if (!o) return;

  // Adding a suboperationplan that was already added
  if (o->owner != this)
    throw LogicException("Operationplan isn't a suboperationplan");

  // Clear owner field
  o->owner = NULL;

  // Remove from the list
  if (o->prevsubopplan)
    o->prevsubopplan->nextsubopplan = o->nextsubopplan;
  else
    firstsubopplan = o->nextsubopplan;
  if (o->nextsubopplan)
    o->nextsubopplan->prevsubopplan = o->prevsubopplan;
  else
    lastsubopplan = o->prevsubopplan;
};


DECLARE_EXPORT bool OperationPlan::operator < (const OperationPlan& a) const
{
  // Different operations
  if (oper != a.oper)
    return *oper < *(a.oper);

  // Different start date
  if (dates.getStart() != a.dates.getStart())
    return dates.getStart() < a.dates.getStart();

  // Sort based on quantity
  return quantity >= a.quantity;
}


DECLARE_EXPORT void OperationPlan::updateSorting()
{
  // Get out right away if this operationplan isn't initialized yet
  if (!(next || prev || oper->last_opplan==this || oper->first_opplan==this))
    return;

  // Verify that we are smaller than the next operationplan
  while (next && !(*this < *next))
  {
    // Swap places with the next
    OperationPlan *tmp = next;
    if (oper->first_opplan == this)
      // New first operationplan
      oper->first_opplan = tmp;
    if (tmp->next) tmp->next->prev = this;
    if (prev) prev->next = tmp;
    tmp->prev = prev;
    next = tmp->next;
    tmp->next = this;
    prev = tmp;
  }

  // Verify that we are bigger than the previous operationplan
  while (prev && !(*prev < *this))
  {
    // Swap places with the previous
    OperationPlan *tmp = prev;
    if (oper->last_opplan == this)
      // New last operationplan
      oper->last_opplan = tmp;
    if (tmp->prev) tmp->prev->next = this;
    if (next) next->prev = tmp;
    prev = tmp->prev;
    tmp->prev = this;
    tmp->next = next;
    next = tmp;
  }

  // Update operation if we have become the first or the last operationplan
  if (!next) oper->last_opplan = this;
  if (!prev) oper->first_opplan = this;
}


DECLARE_EXPORT void OperationPlan::createFlowLoads()
{
  // Has been initialized already, it seems
  if (firstflowplan || firstloadplan) return;

  // Create setup suboperationplans and loadplans
  for (Operation::loadlist::const_iterator g=oper->getLoads().begin();
      g!=oper->getLoads().end(); ++g)
  {
    if (!g->getAlternate()) new LoadPlan(this, &*g);
    if (!g->getSetup().empty() && g->getResource()->getSetupMatrix())
      OperationSetup::setupoperation->createOperationPlan(
      getQuantity(),Date::infinitePast,getDates().getStart(),NULL,this);
  }

  // Create flowplans for flows that are not alternates of another one
  for (Operation::flowlist::const_iterator h=oper->getFlows().begin();
      h!=oper->getFlows().end(); ++h)
    if (!h->getAlternate()) new FlowPlan(this, &*h);
}


DECLARE_EXPORT OperationPlan::~OperationPlan()
{
  // Delete the flowplans
  for (FlowPlanIterator e = beginFlowPlans(); e != endFlowPlans();)
    delete &*(e++);

  // Delete the loadplans
  for (LoadPlanIterator f = beginLoadPlans(); f != endLoadPlans();)
    delete &*(f++);

  // Delete the sub operationplans
  for (OperationPlan *x = firstsubopplan; x; )
  {
    OperationPlan *y = x->nextsubopplan;
    x->owner = NULL; // Need to clear before destroying the suboperationplan
    delete x;
    x = y;
  }

  // Delete also the owner
  if (owner)
  {
    const OperationPlan* o = owner;
    setOwner(NULL);
    delete o;
  }

  // Delete from the list of deliveries
  if (id && dmd) dmd->removeDelivery(this);

  // Delete from the operationplan list
  if (prev)
    // In the middle
    prev->next = next;
  else if (oper->first_opplan == this)
    // First opplan in the list of this operation
    oper->first_opplan = next;
  if (next)
    // In the middle
    next->prev = prev;
  else if (oper->last_opplan == this)
    // Last opplan in the list of this operation
    oper->last_opplan = prev;
}


void DECLARE_EXPORT OperationPlan::setOwner(OperationPlan* o)
{
  // Special case: the same owner is set twice
  if (owner == o) return;
  // Erase the previous owner if there is one
  if (owner) owner->eraseSubOperationPlan(this);
  // Register with the new owner
  if (o) o->addSubOperationPlan(this);
}


void DECLARE_EXPORT OperationPlan::setStart (Date d)
{
  // Locked opplans don't move
  if (getLocked()) return;

  if (!firstsubopplan)   //xxx will need to skip setups
    // No sub operationplans
    oper->setOperationPlanParameters(this,quantity,d,Date::infinitePast);
  else
  {
    // Move all sub-operationplans in an orderly fashion
    bool firstMove = true;
    for (OperationPlan* i = firstsubopplan; i; i = i->nextsubopplan)
    {
      if (i->getDates().getStart() < d || firstMove)
      {
        i->setStart(d);
        firstMove = false;
        d = i->getDates().getEnd();
      }
      else
        // There is sufficient slack between the suboperation plans
        break;
    }
    // Set the dates on the top operationplan
    setStartAndEnd(
      firstsubopplan->getDates().getStart(),
      lastsubopplan->getDates().getEnd()
    );
  }

  // Update flow and loadplans
  update();
}


void DECLARE_EXPORT OperationPlan::setEnd (Date d)
{
  // Locked opplans don't move
  if (getLocked()) return;

  if (!firstsubopplan)   //xxx will need to skip setups
    // No sub operationplans
    oper->setOperationPlanParameters(this,quantity,Date::infinitePast,d);
  else
  {
    // Move all sub-operationplans in an orderly fashion
    bool firstMove = true;
    for (OperationPlan* i = lastsubopplan; i; i = i->prevsubopplan)
    {
      if (i->getDates().getEnd() > d || firstMove)
      {
        i->setEnd(d);
        firstMove = false;
        d = i->getDates().getStart();
      }
      else
        // There is sufficient slack between the suboperations
        break;
    }
    // Set the dates on the top operationplan
    setStartAndEnd(
      firstsubopplan->getDates().getStart(),
      lastsubopplan->getDates().getEnd()
    );
  }

  // Update flow and loadplans
  update();
}


DECLARE_EXPORT double OperationPlan::setQuantity (double f, bool roundDown, bool upd, bool execute)
{
  // No impact on locked operationplans
  if (getLocked()) return quantity;

  // Invalid operationplan: the quantity must be >= 0.
  if (f < 0)
    throw DataException("Operationplans can't have negative quantities");

  // Setting a quantity is only allowed on a top operationplan.
  // One exception: on alternate operations the sizing on the sub-operations is
  // respected.
  if (owner && owner->getOperation()->getType() != *OperationAlternate::metadata)
    return owner->setQuantity(f,roundDown,upd,execute);

  // Compute the correct size for the operationplan
  if (f!=0.0 && getOperation()->getSizeMinimum()>0.0
      && f < getOperation()->getSizeMinimum())
  {
    if (roundDown)
    {
      // Smaller than the minimum quantity, rounding down means... nothing
      if (!execute) return 0.0;
      quantity = 0.0;
      // Update the flow and loadplans, and mark for problem detection
      if (upd) update();
      return 0.0;
    }
    f = getOperation()->getSizeMinimum();
  }
  if (f!=0.0 && f > getOperation()->getSizeMaximum())
  {
    roundDown = true; // force rounddown to stay below the limit
    f = getOperation()->getSizeMaximum();
  }
  if (f!=0.0 && getOperation()->getSizeMultiple()>0.0)
  {
    int mult = static_cast<int> (f / getOperation()->getSizeMultiple()
        + (roundDown ? 0.0 : 0.99999999));
    if (!execute) return mult * getOperation()->getSizeMultiple();
    quantity = mult * getOperation()->getSizeMultiple();
  }
  else
  {
    if (!execute) return f;
    quantity = f;
  }

  // Update the parent of an alternate operationplan
  if (execute && owner
    && owner->getOperation()->getType() == *OperationAlternate::metadata)
  {
    owner->quantity = quantity;
    if (upd) owner->resizeFlowLoadPlans();
  }

  // Apply the same size also to its children
  if (execute && firstsubopplan)
    for (OperationPlan *i = firstsubopplan; i; i = i->nextsubopplan)
    {
      i->quantity = quantity;
      if (upd) i->resizeFlowLoadPlans();
    }

  // Update the flow and loadplans, and mark for problem detection
  if (upd) update();
  return quantity;
}


DECLARE_EXPORT void OperationPlan::resizeFlowLoadPlans()
{
  // Update all flowplans
  for (FlowPlanIterator ee = beginFlowPlans(); ee != endFlowPlans(); ++ee)
    ee->update();

  // Update all loadplans
  for (LoadPlanIterator e = beginLoadPlans(); e != endLoadPlans(); ++e)
    e->update();

  // Allow the operation length to be changed now that the quantity has changed
  // Note that we assume that the end date remains fixed. This assumption makes
  // sense if the operationplan was created to satisfy a demand.
  // It is not valid though when the purpose of the operationplan was to push
  // some material downstream.

  // Notify the demand of the changed delivery
  if (dmd) dmd->setChanged();

  // Update the sorting of the operationplan in the list
  updateSorting();
}


DECLARE_EXPORT void OperationPlan::update()
{
  if (firstsubopplan)  // xxx will need to change for setups
  {
    // Inherit the start and end date of the child operationplans
    dates.setStartAndEnd(
      firstsubopplan->getDates().getStart(),
      lastsubopplan->getDates().getEnd()
    );
    // If at least 1 sub-operationplan is locked, the parent must be locked
    flags &= ~IS_LOCKED; // Clear is_locked flag
    for (OperationPlan* i = firstsubopplan; i; i = i->nextsubopplan)
        if (i->flags & IS_LOCKED)
        {
          flags |= IS_LOCKED;  // Set is_locked flag
          break;
        }
  }

  // Update the flow and loadplans
  resizeFlowLoadPlans();

  // Notify the owner operationplan
  if (owner) owner->update();

  // Mark as changed
  setChanged();
}


DECLARE_EXPORT void OperationPlan::deleteOperationPlans(Operation* o, bool deleteLockedOpplans)
{
  if (!o) return;
  for (OperationPlan *opplan = o->first_opplan; opplan; )
  {
    OperationPlan *tmp = opplan;
    opplan = opplan->next;
    // Note that the deletion of the operationplan also updates the opplan list
    if (deleteLockedOpplans || !tmp->getLocked()) delete tmp;
  }
}


DECLARE_EXPORT void OperationPlan::writer(const MetaCategory* c, XMLOutput* o)
{
  if (!empty())
  {
    o->BeginObject(*c->grouptag);
    for (iterator i=begin(); i!=end(); ++i)
      o->writeElement(*c->typetag, *i);
    o->EndObject(*c->grouptag);
  }
}


DECLARE_EXPORT void OperationPlan::writeElement(XMLOutput *o, const Keyword& tag, mode m) const
{
  // Don't export operationplans of hidden operations
  if (oper->getHidden()) return;

  // Writing a reference
  if (m == REFERENCE)
  {
    o->writeElement
      (tag, Tags::tag_id, id, Tags::tag_operation, oper->getName());
    return;
  }

  if (m != NOHEADER)
    o->BeginObject(tag, Tags::tag_id, id, Tags::tag_operation,oper->getName());

  // The demand reference is only valid for delivery operationplans,
  // and it should only be written if this tag is not being written
  // as part of a demand+delivery tag.
  if (dmd && !dynamic_cast<Demand*>(o->getPreviousObject()))
    o->writeElement(Tags::tag_demand, dmd);

  o->writeElement(Tags::tag_start, dates.getStart());
  o->writeElement(Tags::tag_end, dates.getEnd());
  o->writeElement(Tags::tag_quantity, quantity);
  if (getLocked()) o->writeElement (Tags::tag_locked, getLocked());
  o->writeElement(Tags::tag_owner, owner);

  // Write out the flowplans and their pegging
  if (o->getContentType() == XMLOutput::PLANDETAIL)
  {
    o->BeginObject(Tags::tag_flowplans);
    for (FlowPlanIterator qq = beginFlowPlans(); qq != endFlowPlans(); ++qq)
      qq->writeElement(o, Tags::tag_flowplan);
    o->EndObject(Tags::tag_flowplans);
  }

  o->EndObject(tag);
}


DECLARE_EXPORT void OperationPlan::beginElement (XMLInput& pIn, const Attribute& pAttr)
{
  if (pAttr.isA (Tags::tag_demand))
    pIn.readto( Demand::reader(Demand::metadata,pIn.getAttributes()) );
  else if (pAttr.isA(Tags::tag_owner))
    pIn.readto(createOperationPlan(metadata,pIn.getAttributes()));
  else if (pAttr.isA(Tags::tag_flowplans))
    pIn.IgnoreElement();
}


DECLARE_EXPORT void OperationPlan::endElement (XMLInput& pIn, const Attribute& pAttr, const DataElement& pElement)
{
  // Note that the fields have been ordered more or less in the order
  // of their expected frequency.
  // Note that id and operation are handled already during the
  // operationplan creation. They don't need to be handled here...
  if (pAttr.isA(Tags::tag_quantity))
    pElement >> quantity;
  else if (pAttr.isA(Tags::tag_start))
    dates.setStart(pElement.getDate());
  else if (pAttr.isA(Tags::tag_end))
    dates.setEnd(pElement.getDate());
  else if (pAttr.isA(Tags::tag_owner) && !pIn.isObjectEnd())
  {
    OperationPlan* o = dynamic_cast<OperationPlan*>(pIn.getPreviousObject());
    if (o) setOwner(o);
  }
  else if (pIn.isObjectEnd())
  {
    // Initialize the operationplan
    if (!instantiate())
      // Initialization failed and the operationplan is deleted
      pIn.invalidateCurrentObject();
  }
  else if (pAttr.isA (Tags::tag_demand))
  {
    Demand * d = dynamic_cast<Demand*>(pIn.getPreviousObject());
    if (d) d->addDelivery(this);
    else throw LogicException("Incorrect object type during read operation");
  }
  else if (pAttr.isA(Tags::tag_locked))
    setLocked(pElement.getBool());
}


DECLARE_EXPORT void OperationPlan::setLocked(bool b)
{
  if (b)
    flags |= IS_LOCKED;
  else
    flags &= ~IS_LOCKED;
  for (OperationPlan *x = firstsubopplan; x; x = x->nextsubopplan)
    x->setLocked(b);
  update();
}


DECLARE_EXPORT void OperationPlan::setDemand(Demand* l)
{
  // No change
  if (l==dmd) return;

  // Unregister from previous lot
  if (dmd) dmd->removeDelivery(this);

  // Register the new demand and mark it changed
  dmd = l;
  if (l) l->setChanged();
}



PyObject* OperationPlan::create(PyTypeObject* pytype, PyObject* args, PyObject* kwds)
{
  try
  {
    // Find or create the C++ object
    PythonAttributeList atts(kwds);
    Object* x = createOperationPlan(OperationPlan::metadata,atts);

    // Iterate over extra keywords, and set attributes.   @todo move this responsability to the readers...
    if (x)
    {
      PyObject *key, *value;
      Py_ssize_t pos = 0;
      while (PyDict_Next(kwds, &pos, &key, &value))
      {
        PythonObject field(value);
        Attribute attr(PyString_AsString(key));
        if (!attr.isA(Tags::tag_operation) && !attr.isA(Tags::tag_id) && !attr.isA(Tags::tag_action))
        {
          int result = x->setattro(attr, field);
          if (result && !PyErr_Occurred())
            PyErr_Format(PyExc_AttributeError,
              "attribute '%s' on '%s' can't be updated",
              PyString_AsString(key), x->ob_type->tp_name);
        }
      };
    }

    if (x && !static_cast<OperationPlan*>(x)->instantiate())
      return NULL;
    return x;
  }
  catch (...)
  {
    PythonType::evalException();
    return NULL;
  }
}


DECLARE_EXPORT PyObject* OperationPlan::getattro(const Attribute& attr)
{
  if (attr.isA(Tags::tag_id))
    return PythonObject(getIdentifier());
  if (attr.isA(Tags::tag_operation))
    return PythonObject(getOperation());
  if (attr.isA(Tags::tag_flowplans))
    return new frepple::FlowPlanIterator(this);
  if (attr.isA(Tags::tag_loadplans))
    return new frepple::LoadPlanIterator(this);
  if (attr.isA(Tags::tag_quantity))
    return PythonObject(getQuantity());
  if (attr.isA(Tags::tag_start))
    return PythonObject(getDates().getStart());
  if (attr.isA(Tags::tag_end))
    return PythonObject(getDates().getEnd());
  if (attr.isA(Tags::tag_demand))
    return PythonObject(getDemand());
  if (attr.isA(Tags::tag_locked))
    return PythonObject(getLocked());
  if (attr.isA(Tags::tag_owner))
    return PythonObject(getOwner());
  if (attr.isA(Tags::tag_operationplans))
    return new OperationPlanIterator(this);
  if (attr.isA(Tags::tag_hidden))
    return PythonObject(getHidden());
  return NULL;
}


DECLARE_EXPORT int OperationPlan::setattro(const Attribute& attr, const PythonObject& field)
{
  if (attr.isA(Tags::tag_quantity))
    setQuantity(field.getDouble());
  else if (attr.isA(Tags::tag_start))
    setStart(field.getDate());
  else if (attr.isA(Tags::tag_end))
    setEnd(field.getDate());
  else if (attr.isA(Tags::tag_locked))
    setLocked(field.getBool());
  else if (attr.isA(Tags::tag_demand))
  {
    if (!field.check(Demand::metadata))
    {
      PyErr_SetString(PythonDataException, "operationplan demand must be of type demand");
      return -1;
    }
    Demand* y = static_cast<Demand*>(static_cast<PyObject*>(field));
    setDemand(y);
  }
  else if (attr.isA(Tags::tag_owner))
  {
    if (!field.check(OperationPlan::metadata))
    {
      PyErr_SetString(PythonDataException, "operationplan demand must be of type demand");
      return -1;
    }
    OperationPlan* y = static_cast<OperationPlan*>(static_cast<PyObject*>(field));
    setOwner(y);
  }
  else
    return -1;
  return 0;
}

} // end namespace
