
// Compiler implementation of the D programming language
// Copyright (c) 1999-2010 by Digital Mars
// All Rights Reserved
// written by Walter Bright
// http://www.digitalmars.com
// License for redistribution is by either the Artistic License
// in artistic.txt, or the GNU General Public License in gnu.txt.
// See the included readme.txt for details.

/* NOTE: This file has been patched from the original DMD distribution to
   work with the GDC compiler.

   Modified by David Friedman, December 2006
*/

#include <stdio.h>
#include <assert.h>

#include "root.h"
#include "aggregate.h"
#include "scope.h"
#include "mtype.h"
#include "declaration.h"
#include "module.h"
#include "id.h"
#include "statement.h"
#include "template.h"

/********************************* AggregateDeclaration ****************************/

AggregateDeclaration::AggregateDeclaration(Loc loc, Identifier *id)
    : ScopeDsymbol(id)
{
    this->loc = loc;

    storage_class = 0;
    protection = PROTpublic;
    type = NULL;
    handle = NULL;
    structsize = 0;		// size of struct
    alignsize = 0;		// size of struct for alignment purposes
    structalign = 0;		// struct member alignment in effect
    hasUnions = 0;
    sizeok = 0;			// size not determined yet
    isdeprecated = 0;
    inv = NULL;
    aggNew = NULL;
    aggDelete = NULL;

    attributes = NULL;

    stag = NULL;
    sinit = NULL;
    isnested = 0;
    vthis = NULL;

#if DMDV2
    ctor = NULL;
    defaultCtor = NULL;
    aliasthis = NULL;
#endif
    dtor = NULL;
}

enum PROT AggregateDeclaration::prot()
{
    return protection;
}

void AggregateDeclaration::semantic2(Scope *sc)
{
    //printf("AggregateDeclaration::semantic2(%s)\n", toChars());
    if (scope && members)
    {	error("has forward references");
	return;
    }
    if (members)
    {
	sc = sc->push(this);
	for (size_t i = 0; i < members->dim; i++)
	{
	    Dsymbol *s = (Dsymbol *)members->data[i];
	    s->semantic2(sc);
	}
	sc->pop();
    }
}

void AggregateDeclaration::semantic3(Scope *sc)
{   int i;

    //printf("AggregateDeclaration::semantic3(%s)\n", toChars());
    if (members)
    {
	sc = sc->push(this);
	for (i = 0; i < members->dim; i++)
	{
	    Dsymbol *s = (Dsymbol *)members->data[i];
	    s->semantic3(sc);
	}
	sc->pop();
    }
}

void AggregateDeclaration::inlineScan()
{   int i;

    //printf("AggregateDeclaration::inlineScan(%s)\n", toChars());
    if (members)
    {
	for (i = 0; i < members->dim; i++)
	{
	    Dsymbol *s = (Dsymbol *)members->data[i];
	    //printf("inline scan aggregate symbol '%s'\n", s->toChars());
	    s->inlineScan();
	}
    }
}

target_size_t AggregateDeclaration::size(Loc loc)
{
    //printf("AggregateDeclaration::size() = %d\n", structsize);
    if (!members)
	error(loc, "unknown size");
    if (sizeok != 1)
    {	error(loc, "no size yet for forward reference");
	//*(char*)0=0;
    }
    return structsize;
}

Type *AggregateDeclaration::getType()
{
    return type;
}

int AggregateDeclaration::isDeprecated()
{
    return isdeprecated;
}

/****************************
 * Do byte or word alignment as necessary.
 * Align sizes of 0, as we may not know array sizes yet.
 */

void AggregateDeclaration::alignmember(
	target_size_t salign,	// struct alignment that is in effect
	target_size_t size,	// alignment requirement of field
	target_size_t *poffset)
{
    //printf("salign = %d, size = %d, offset = %d\n",salign,size,offset);
    if (salign > 1)
    {
	assert(size != 3);
	int sa = size;
	if (sa == 0 || salign < sa)
	    sa = salign;
	*poffset = (*poffset + sa - 1) & ~(sa - 1);
    }
    //printf("result = %d\n",offset);
}


void AggregateDeclaration::addField(Scope *sc, VarDeclaration *v)
{
    target_size_t memsize;	// size of member
    target_size_t memalignsize;	// size of member for alignment purposes
    target_size_t xalign;	// alignment boundaries

    //printf("AggregateDeclaration::addField('%s') %s\n", v->toChars(), toChars());
    assert(!(v->storage_class & (STCstatic | STCextern | STCparameter | STCtls)));

    // Check for forward referenced types which will fail the size() call
    Type *t = v->type->toBasetype();
    if (v->storage_class & STCref)
    {	// References are the size of a pointer
	t = Type::tvoidptr;
    }
    if (t->ty == Tstruct /*&& isStructDeclaration()*/)
    {	TypeStruct *ts = (TypeStruct *)t;
#if DMDV2
	if (ts->sym == this)
	{
	    error("cannot have field %s with same struct type", v->toChars());
	}
#endif

	if (ts->sym->sizeok != 1)
	{
	    sizeok = 2;		// cannot finish; flag as forward referenced
	    return;
	}
    }
    if (t->ty == Tident)
    {
	sizeok = 2;		// cannot finish; flag as forward referenced
	return;
    }

    memsize = t->size(loc);
    memalignsize = t->alignsize();
    xalign = t->memalign(sc->structalign);
    alignmember(xalign, memalignsize, &sc->offset);
    v->offset = sc->offset;
    sc->offset += memsize;
    if (sc->offset > structsize)
	structsize = sc->offset;
    if (sc->structalign < memalignsize)
	memalignsize = sc->structalign;
    if (alignsize < memalignsize)
	alignsize = memalignsize;
    //printf("\talignsize = %d\n", alignsize);

    v->storage_class |= STCfield;
    //printf(" addField '%s' to '%s' at offset %d, size = %d\n", v->toChars(), toChars(), v->offset, memsize);
    fields.push(v);
}


/****************************************
 * Returns !=0 if there's an extra member which is the 'this'
 * pointer to the enclosing context (enclosing aggregate or function)
 */

int AggregateDeclaration::isNested()
{
    return isnested;
}


/********************************* StructDeclaration ****************************/

StructDeclaration::StructDeclaration(Loc loc, Identifier *id)
    : AggregateDeclaration(loc, id)
{
    zeroInit = 0;	// assume false until we do semantic processing
#if DMDV2
    hasIdentityAssign = 0;
    cpctor = NULL;
    postblit = NULL;
    eq = NULL;
#endif

    // For forward references
    type = new TypeStruct(this);
}

Dsymbol *StructDeclaration::syntaxCopy(Dsymbol *s)
{
    StructDeclaration *sd;

    if (s)
	sd = (StructDeclaration *)s;
    else
	sd = new StructDeclaration(loc, ident);
    ScopeDsymbol::syntaxCopy(sd);
    return sd;
}

void StructDeclaration::semantic(Scope *sc)
{
    Scope *sc2;

    //printf("+StructDeclaration::semantic(this=%p, '%s', sizeok = %d)\n", this, toChars(), sizeok);

    //static int count; if (++count == 20) halt();

    assert(type);
    if (!members)			// if forward reference
	return;

    if (symtab)
    {   if (sizeok == 1 || !scope)
	{   //printf("already completed\n");
	    scope = NULL;
            return;             // semantic() already completed
	}
    }
    else
        symtab = new DsymbolTable();

    Scope *scx = NULL;
    if (scope)
    {   sc = scope;
        scx = scope;            // save so we don't make redundant copies
        scope = NULL;
    }
#ifdef IN_GCC
    methods.setDim(0);
#endif

    unsigned dprogress_save = Module::dprogress;

    parent = sc->parent;
    type = type->semantic(loc, sc);
#if STRUCTTHISREF
    handle = type;
#else
    handle = type->pointerTo();
#endif
    structalign = sc->structalign;
    protection = sc->protection;
    storage_class |= sc->stc;
    if (sc->stc & STCdeprecated)
	isdeprecated = 1;
    assert(!isAnonymous());
    if (sc->stc & STCabstract)
	error("structs, unions cannot be abstract");
#if DMDV2
    if (storage_class & STCimmutable)
        type = type->invariantOf();
    else if (storage_class & STCconst)
        type = type->constOf();
    else if (storage_class & STCshared)
        type = type->sharedOf();
#endif
    if (attributes)
	attributes->append(sc->attributes);
    else
	attributes = sc->attributes;

    if (sizeok == 0)		// if not already done the addMember step
    {
	int hasfunctions = 0;
	for (int i = 0; i < members->dim; i++)
	{
	    Dsymbol *s = (Dsymbol *)members->data[i];
	    //printf("adding member '%s' to '%s'\n", s->toChars(), this->toChars());
	    s->addMember(sc, this, 1);
	    if (s->isFuncDeclaration())
		hasfunctions = 1;
	}

	// If nested struct, add in hidden 'this' pointer to outer scope
	if (hasfunctions && !(storage_class & STCstatic))
        {   Dsymbol *s = toParent2();
            if (s)
            {
                AggregateDeclaration *ad = s->isAggregateDeclaration();
                FuncDeclaration *fd = s->isFuncDeclaration();

		TemplateInstance *ti;
                if (ad && (ti = ad->parent->isTemplateInstance()) != NULL && ti->isnested || fd)
                {   isnested = 1;
                    Type *t;
                    if (ad)
                        t = ad->handle;
                    else if (fd)
                    {   AggregateDeclaration *ad = fd->isMember2();
                        if (ad)
                            t = ad->handle;
                        else
			    t = Type::tvoidptr;
                    }
                    else
                        assert(0);
		    if (t->ty == Tstruct)
			t = Type::tvoidptr;	// t should not be a ref type
                    assert(!vthis);
                    vthis = new ThisDeclaration(loc, t);
		    //vthis->storage_class |= STCref;
                    members->push(vthis);
                }
            }
        }
    }

    sizeok = 0;
    sc2 = sc->push(this);
    sc2->stc &= storage_class & STC_TYPECTOR;
    sc2->attributes = NULL;
    sc2->parent = this;
    if (isUnionDeclaration())
	sc2->inunion = 1;
    sc2->protection = PROTpublic;
    sc2->explicitProtection = 0;

    int members_dim = members->dim;

    /* Set scope so if there are forward references, we still might be able to
     * resolve individual members like enums.
     */
    for (int i = 0; i < members_dim; i++)
    {	Dsymbol *s = (Dsymbol *)members->data[i];
	/* There are problems doing this in the general case because
	 * Scope keeps track of things like 'offset'
	 */
	if (s->isEnumDeclaration() || (s->isAggregateDeclaration() && s->ident))
	{
	    //printf("setScope %s %s\n", s->kind(), s->toChars());
	    s->setScope(sc2);
	}
    }

    for (int i = 0; i < members_dim; i++)
    {
	Dsymbol *s = (Dsymbol *)members->data[i];
	s->semantic(sc2);
	if (isUnionDeclaration())
	    sc2->offset = 0;
#if 0
	if (sizeok == 2)
	{   //printf("forward reference\n");
	    break;
	}
#endif
	Type *t;
	if (s->isDeclaration() &&
	    (t = s->isDeclaration()->type) != NULL &&
	    t->toBasetype()->ty == Tstruct)
	{   StructDeclaration *sd = (StructDeclaration *)t->toDsymbol(sc);
	    if (sd->isnested)
		error("inner struct %s cannot be a field", sd->toChars());
	}
    }

#if DMDV1
    /* This doesn't work for DMDV2 because (ref S) and (S) parameter
     * lists will overload the same.
     */
    /* The TypeInfo_Struct is expecting an opEquals and opCmp with
     * a parameter that is a pointer to the struct. But if there
     * isn't one, but is an opEquals or opCmp with a value, write
     * another that is a shell around the value:
     *	int opCmp(struct *p) { return opCmp(*p); }
     */

    TypeFunction *tfeqptr;
    {
	Parameters *arguments = new Parameters;
	Parameter *arg = new Parameter(STCin, handle, Id::p, NULL);

	arguments->push(arg);
	tfeqptr = new TypeFunction(arguments, Type::tint32, 0, LINKd);
	tfeqptr = (TypeFunction *)tfeqptr->semantic(0, sc);
    }

    TypeFunction *tfeq;
    {
	Parameters *arguments = new Parameters;
	Parameter *arg = new Parameter(STCin, type, NULL, NULL);

	arguments->push(arg);
	tfeq = new TypeFunction(arguments, Type::tint32, 0, LINKd);
	tfeq = (TypeFunction *)tfeq->semantic(0, sc);
    }

    Identifier *id = Id::eq;
    for (int i = 0; i < 2; i++)
    {
	Dsymbol *s = search_function(this, id);
	FuncDeclaration *fdx = s ? s->isFuncDeclaration() : NULL;
	if (fdx)
	{   FuncDeclaration *fd = fdx->overloadExactMatch(tfeqptr);
	    if (!fd)
	    {	fd = fdx->overloadExactMatch(tfeq);
		if (fd)
		{   // Create the thunk, fdptr
		    FuncDeclaration *fdptr = new FuncDeclaration(loc, loc, fdx->ident, STCundefined, tfeqptr);
		    Expression *e = new IdentifierExp(loc, Id::p);
		    e = new PtrExp(loc, e);
		    Expressions *args = new Expressions();
		    args->push(e);
		    e = new IdentifierExp(loc, id);
		    e = new CallExp(loc, e, args);
		    fdptr->fbody = new ReturnStatement(loc, e);
		    ScopeDsymbol *s = fdx->parent->isScopeDsymbol();
		    assert(s);
		    s->members->push(fdptr);
		    fdptr->addMember(sc, s, 1);
		    fdptr->semantic(sc2);
		}
	    }
	}

	id = Id::cmp;
    }
#endif
#if DMDV2
    /* Try to find the opEquals function. Build it if necessary.
     */
    TypeFunction *tfeqptr;
    {   // bool opEquals(const T*) const;
        Parameters *parameters = new Parameters;
#if STRUCTTHISREF
        // bool opEquals(ref const T) const;
        Parameter *param = new Parameter(STCref, type->constOf(), NULL, NULL);
#else
        // bool opEquals(const T*) const;
        Parameter *param = new Parameter(STCin, type->pointerTo(), NULL, NULL);
#endif

        parameters->push(param);
        tfeqptr = new TypeFunction(parameters, Type::tbool, 0, LINKd);
        tfeqptr->mod = MODconst;
        tfeqptr = (TypeFunction *)tfeqptr->semantic(0, sc2);

	Dsymbol *s = search_function(this, Id::eq);
	FuncDeclaration *fdx = s ? s->isFuncDeclaration() : NULL;
	if (fdx)
	{
	    eq = fdx->overloadExactMatch(tfeqptr);
	    if (!eq)
		fdx->error("type signature should be %s not %s", tfeqptr->toChars(), fdx->type->toChars());
	}

	if (!eq)
	    eq = buildOpEquals(sc2);
    }

    dtor = buildDtor(sc2);
    postblit = buildPostBlit(sc2);
    cpctor = buildCpCtor(sc2);
    buildOpAssign(sc2);
#endif

    sc2->pop();

    if (sizeok == 2)
    {	// semantic() failed because of forward references.
	// Unwind what we did, and defer it for later
	fields.setDim(0);
	structsize = 0;
	alignsize = 0;
	structalign = 0;

	scope = scx ? scx : new Scope(*sc);
	scope->setNoFree();
	scope->module->addDeferredSemantic(this);

	Module::dprogress = dprogress_save;
	//printf("\tdeferring %s\n", toChars());
	return;
    }

    // 0 sized struct's are set to 1 byte
    if (structsize == 0)
    {
	structsize = 1;
	alignsize = 1;
    }

    // Round struct size up to next alignsize boundary.
    // This will ensure that arrays of structs will get their internals
    // aligned properly.
    structsize = (structsize + alignsize - 1) & ~(alignsize - 1);

    sizeok = 1;
    Module::dprogress++;

    //printf("-StructDeclaration::semantic(this=%p, '%s')\n", this, toChars());

    // Determine if struct is all zeros or not
    zeroInit = 1;
    for (int i = 0; i < fields.dim; i++)
    {
	Dsymbol *s = (Dsymbol *)fields.data[i];
	VarDeclaration *vd = s->isVarDeclaration();
	if (vd && !vd->isDataseg())
	{
	    if (vd->init)
	    {
		// Should examine init to see if it is really all 0's
		zeroInit = 0;
		break;
	    }
	    else
	    {
		if (!vd->type->isZeroInit(loc))
		{
		    zeroInit = 0;
		    break;
		}
	    }
	}
    }

    /* Look for special member functions.
     */
#if DMDV2
    ctor = search(0, Id::ctor, 0);
#endif
    inv =    (InvariantDeclaration *)search(0, Id::classInvariant, 0);
    aggNew =       (NewDeclaration *)search(0, Id::classNew,       0);
    aggDelete = (DeleteDeclaration *)search(0, Id::classDelete,    0);

    if (sc->func)
    {
	semantic2(sc);
	semantic3(sc);
    }
}

Dsymbol *StructDeclaration::search(Loc loc, Identifier *ident, int flags)
{
    //printf("%s.StructDeclaration::search('%s')\n", toChars(), ident->toChars());

    if (scope)
    	semantic(scope);

    if (!members || !symtab)
    {
	error("is forward referenced when looking for '%s'", ident->toChars());
	return NULL;
    }

    return ScopeDsymbol::search(loc, ident, flags);
}

void StructDeclaration::toCBuffer(OutBuffer *buf, HdrGenState *hgs)
{   int i;

    buf->printf("%s ", kind());
    if (!isAnonymous())
	buf->writestring(toChars());
    if (!members)
    {
	buf->writeByte(';');
	buf->writenl();
	return;
    }
    buf->writenl();
    buf->writeByte('{');
    buf->writenl();
    for (i = 0; i < members->dim; i++)
    {
	Dsymbol *s = (Dsymbol *)members->data[i];

	buf->writestring("    ");
	s->toCBuffer(buf, hgs);
    }
    buf->writeByte('}');
    buf->writenl();
}


const char *StructDeclaration::kind()
{
    return "struct";
}

/********************************* UnionDeclaration ****************************/

UnionDeclaration::UnionDeclaration(Loc loc, Identifier *id)
    : StructDeclaration(loc, id)
{
}

Dsymbol *UnionDeclaration::syntaxCopy(Dsymbol *s)
{
    UnionDeclaration *ud;

    if (s)
	ud = (UnionDeclaration *)s;
    else
	ud = new UnionDeclaration(loc, ident);
    StructDeclaration::syntaxCopy(ud);
    return ud;
}


const char *UnionDeclaration::kind()
{
    return "union";
}


