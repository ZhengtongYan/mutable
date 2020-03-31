#include "IR/QueryGraph.hpp"

#include "catalog/Schema.hpp"
#include "parse/AST.hpp"
#include "parse/ASTDumper.hpp"
#include "parse/ASTVisitor.hpp"
#include "util/macro.hpp"
#include <set>
#include <unordered_map>


using namespace db;


Query::~Query() { delete query_graph_; };

/** Helper structure to extract the tables required by an expression. */
struct GetTables : ConstASTExprVisitor
{
    private:
    std::set<const char*> tables_;

    public:
    GetTables() { }

    std::set<const char*> get() { return std::move(tables_); }

    using ConstASTExprVisitor::operator();
    void operator()(Const<ErrorExpr>&) override { unreachable("graph must not contain errors"); }

    void operator()(Const<Designator> &e) override { tables_.emplace(e.get_table_name()); }

    void operator()(Const<Constant>&) override { /* nothing to be done */ }

    void operator()(Const<FnApplicationExpr> &e) override {
        (*this)(*e.fn);
        for (auto arg : e.args)
            (*this)(*arg);
    }

    void operator()(Const<UnaryExpr> &e) override { (*this)(*e.expr); }

    void operator()(Const<BinaryExpr> &e) override { (*this)(*e.lhs); (*this)(*e.rhs); }
};

/** Given a clause of a CNF formula, compute the tables that are required by this clause. */
auto get_tables(const cnf::Clause &clause)
{
    using std::begin, std::end;
    GetTables GT;
    for (auto p : clause)
        GT(*p.expr());
    return GT.get();
}

/** Helper structure to extract the aggregate functions. */
struct GetAggregates : ConstASTExprVisitor, ConstASTClauseVisitor, ConstASTStmtVisitor
{
    private:
    std::vector<const Expr*> aggregates_;

    public:
    GetAggregates() { }

    auto get() { return std::move(aggregates_); }

    using ConstASTExprVisitor::Const;

    /*----- Expressions ----------------------------------------------------------------------------------------------*/
    using ConstASTExprVisitor::operator();
    void operator()(Const<ErrorExpr>&) override { unreachable("graph must not contain errors"); }

    void operator()(Const<Designator>&) override { /* nothing to be done */ }
    void operator()(Const<Constant>&) override { /* nothing to be done */ }
    void operator()(Const<UnaryExpr> &e) override { (*this)(*e.expr); }
    void operator()(Const<BinaryExpr> &e) override { (*this)(*e.lhs); (*this)(*e.rhs); }

    void operator()(Const<FnApplicationExpr> &e) override {
        insist(e.has_function());
        if (e.get_function().is_aggregate()) { // test that this is an aggregation
            using std::find_if, std::to_string;
            std::string str = to_string(e);
            auto exists = [&](const Expr *agg) { return to_string(*agg) == str; };
            if (find_if(aggregates_.begin(), aggregates_.end(), exists) == aggregates_.end()) // test if already present
                aggregates_.push_back(&e);
        }
    }

    /*----- Clauses --------------------------------------------------------------------------------------------------*/
    using ConstASTClauseVisitor::operator();
    void operator()(Const<ErrorClause>&) override { unreachable("not implemented"); }
    void operator()(Const<FromClause>&) override { unreachable("not implemented"); }
    void operator()(Const<WhereClause>&) override { unreachable("not implemented"); }
    void operator()(Const<GroupByClause>&) override { unreachable("not implemented"); }
    void operator()(Const<LimitClause>&) override { unreachable("not implemented"); }


    void operator()(Const<SelectClause> &c) override {
        for (auto s : c.select)
            (*this)(*s.first);
    }

    void operator()(Const<HavingClause> &c) override { (*this)(*c.having); }

    void operator()(Const<OrderByClause> &c) override {
        for (auto o : c.order_by)
            (*this)(*o.first);
    }

    /*----- Statements -----------------------------------------------------------------------------------------------*/
    using ConstASTStmtVisitor::operator();
    void operator()(Const<ErrorStmt>&) override { unreachable("not implemented"); }
    void operator()(Const<EmptyStmt>&) override { unreachable("not implemented"); }
    void operator()(Const<CreateDatabaseStmt>&) override { unreachable("not implemented"); }
    void operator()(Const<UseDatabaseStmt>&) override { unreachable("not implemented"); }
    void operator()(Const<CreateTableStmt>&) override { unreachable("not implemented"); }
    void operator()(Const<InsertStmt>&) override { unreachable("not implemented"); }
    void operator()(Const<UpdateStmt>&) override { unreachable("not implemented"); }
    void operator()(Const<DeleteStmt>&) override { unreachable("not implemented"); }
    void operator()(Const<DSVImportStmt>&) override { }

    void operator()(Const<SelectStmt> &s) override {
        if (s.having) (*this)(*s.having);
        (*this)(*s.select);
        if (s.order_by) (*this)(*s.order_by);
    }
};

/** Given a select statement, extract the aggregates to compute while grouping. */
auto get_aggregates(const Stmt &stmt)
{
    GetAggregates GA;
    GA(stmt);
    return GA.get();
}

/*======================================================================================================================
 * GraphBuilder
 *
 * An AST Visitor that constructs the query graph.
 *====================================================================================================================*/

struct db::GraphBuilder : ConstASTStmtVisitor
{
    private:
    std::unique_ptr<QueryGraph> graph_; ///< the constructed query graph
    std::unordered_map<const char*, DataSource*> aliases; ///< maps aliases to data sources

    public:
    GraphBuilder() : graph_(std::make_unique<QueryGraph>()) { }

    std::unique_ptr<QueryGraph> get() { return std::move(graph_); }

    using ConstASTStmtVisitor::operator();
    void operator()(Const<Stmt> &s) { s.accept(*this); }
    void operator()(Const<ErrorStmt>&) { unreachable("graph must not contain errors"); }

    void operator()(Const<EmptyStmt>&) { /* nothing to be done */ }

    void operator()(Const<CreateDatabaseStmt>&) {
        /* TODO: implement */
    }

    void operator()(Const<UseDatabaseStmt>&) {
        /* TODO: implement */
    }

    void operator()(Const<CreateTableStmt>&) {
        /* TODO: implement */
    }

    void operator()(Const<SelectStmt> &s) {
        /* Compute CNF of WHERE clause. */
        cnf::CNF cnf;
        if (s.where) {
            auto where = as<WhereClause>(s.where);
            cnf = cnf::to_CNF(*where->where);
        }

        /* Create data sources. */
        if (s.from) {
            auto from = as<FromClause>(s.from);
            for (auto &tbl : from->from) {
                if (auto tok = std::get_if<Token>(&tbl.source)) {
                    /* Create a new base table. */
                    insist(tbl.has_table());
                    Token alias = tbl.alias ? tbl.alias : *tok;
                    auto base = new BaseTable(graph_->sources_.size(), alias.text, tbl.table());
                    aliases.emplace(alias.text, base);
                    graph_->sources_.emplace_back(base);
                } else if (auto stmt = std::get_if<Stmt*>(&tbl.source)) {
                    insist(tbl.alias.text, "every nested statement requires an alias");
                    if (auto select = cast<SelectStmt>(*stmt)) {
                        /* Create a graph for the sub query. */
                        GraphBuilder builder;
                        builder(*select);
                        auto graph = builder.get();
                        auto q = new Query(graph_->sources_.size(), tbl.alias.text, graph.release());
                        insist(tbl.alias);
                        aliases.emplace(tbl.alias.text, q);
                        graph_->sources_.emplace_back(q);
                    } else
                        unreachable("invalid variant");
                } else {
                    unreachable("invalid variant");
                }
            }
        }

        /* Dissect CNF into joins and filters. */
        for (auto &clause : cnf) {
            auto tables = get_tables(clause);
            if (tables.size() == 0) {
                /* This clause is a filter and constant.  It applies to all data sources. */
                for (auto &alias : aliases)
                    alias.second->update_filter(cnf::CNF{clause});
            } else if (tables.size() == 1) {
                /* This clause is a filter condition. */
                auto t = *begin(tables);
                auto ds = aliases.at(t);
                ds->update_filter(cnf::CNF({clause}));
            } else {
                /* This clause is a join condition. */
                Join::sources_t sources;
                for (auto t : tables)
                    sources.emplace_back(aliases.at(t));
                auto J = graph_->joins_.emplace_back(new Join(cnf::CNF({clause}), sources));
                for (auto ds : J->sources())
                    ds->add_join(J);
            }
        }

        /* Add groups. */
        if (s.group_by) {
            auto G = as<GroupByClause>(s.group_by);
            graph_->group_by_.insert(graph_->group_by_.begin(), G->group_by.begin(), G->group_by.end());
        }

        /* Add aggregates. */
        graph_->aggregates_ = get_aggregates(s);

        /* Implement HAVING as a regular selection filter on a sub query. */
        if (s.having) {
            auto H = as<HavingClause>(s.having);
            auto sub = new Query(0, "HAVING", graph_.release());
            sub->update_filter(cnf::to_CNF(*H->having));
            graph_ = std::make_unique<QueryGraph>();
            graph_->sources_.emplace_back(sub);
        }

        /* Add projections */
        {
            auto S = as<SelectClause>(s.select);
            graph_->projection_is_anti_ = S->select_all;
            for (auto s : S->select)
                graph_->projections_.emplace_back(s.first, s.second.text);
        }

        /* Add order by. */
        if (s.order_by) {
            auto O = as<OrderByClause>(s.order_by);
            for (auto o : O->order_by)
                graph_->order_by_.emplace_back(o.first, o.second);
        }

        /* Add limit. */
        if (s.limit) {
            auto L = as<LimitClause>(s.limit);
            errno = 0;
            graph_->limit_.limit = strtoull(L->limit.text, nullptr, 10);
            insist(errno == 0);
            if (L->offset) {
                graph_->limit_.offset = strtoull(L->offset.text, nullptr, 10);
                insist(errno == 0);
            }
        }
    }

    void operator()(Const<InsertStmt>&) {
        /* TODO: implement */
    }

    void operator()(Const<UpdateStmt>&) {
        /* TODO: implement */
    }

    void operator()(Const<DeleteStmt>&) {
        /* TODO: implement */
    }

    void operator()(Const<DSVImportStmt>&) { }
};

/*======================================================================================================================
 * QueryGraph
 *====================================================================================================================*/

QueryGraph::~QueryGraph()
{
    for (auto src : sources_)
        delete src;
    for (auto j : joins_)
        delete j;
}

std::unique_ptr<QueryGraph> QueryGraph::Build(const Stmt &stmt)
{
    GraphBuilder builder;
    builder(stmt);
    return builder.get();
}

void QueryGraph::dot(std::ostream &out) const
{
    out << "graph query_graph\n{\n"
        << "    forcelabels=true;\n"
        << "    overlap=false;\n"
        << "    labeljust=\"l\";\n"
        << "    graph [compound=true];\n"
        << "    graph [fontname = \"DejaVu Sans\"];\n"
        << "    node [fontname = \"DejaVu Sans\"];\n"
        << "    edge [fontname = \"DejaVu Sans\"];\n";

    dot_recursive(out);

    out << '}' << std::endl;
}

void QueryGraph::dot_recursive(std::ostream &out) const
{
#define q(X) '"' << X << '"' // quote
#define id(X) q(std::hex << &X << std::dec) // convert virtual address to identifier
    for (auto ds : sources()) {
        if (auto q = cast<Query>(ds)) {
            q->query_graph()->dot_recursive(out);
        }
    }

    out << "\n  subgraph cluster_" << this << " {\n";

    for (auto ds : sources_) {
        out << "    " << id(*ds) << " [label=<<B>" << ds->alias() << "</B>";
        if (ds->filter().size())
            out << "<BR/><FONT COLOR=\"0.0 0.0 0.25\" POINT-SIZE=\"10\">"
                << html_escape(to_string(ds->filter()))
                << "</FONT>";
        out << ">,style=filled,fillcolor=\"0.0 0.0 0.8\"];\n";
        if (auto q = cast<Query>(ds))
            out << "  " << id(*ds) << " -- \"cluster_" << q->query_graph() << "\";\n";
    }

    for (auto j : joins_) {
        out << "    " << id(*j) << " [label=<" << html_escape(to_string(j->condition())) << ">,style=filled,fillcolor=\"0.0 0.0 0.95\"];\n";
        for (auto ds : j->sources())
            out << "    " << id(*j) << " -- " << id(*ds) << ";\n";
    }

    out << "    label=<"
        << "<TABLE BORDER=\"0\" CELLPADDING=\"0\" CELLSPACING=\"0\">\n";

    /* Limit */
    if (limit_.limit != 0 or limit_.offset != 0) {
        out << "             <TR><TD ALIGN=\"LEFT\">\n"
            << "               <B>λ</B><FONT POINT-SIZE=\"9\">"
            << limit_.limit << ", " << limit_.offset
            << "</FONT>\n"
            << "             </TD></TR>\n";
    }

    /* Order by */
    if (order_by_.size()) {
        out << "             <TR><TD ALIGN=\"LEFT\">\n"
            << "               <B>ω</B><FONT POINT-SIZE=\"9\">";
        for (auto it = order_by_.begin(), end = order_by_.end(); it != end; ++it) {
            if (it != order_by_.begin()) out << ", ";
            out << html_escape(to_string(*it->first));
            out << ' ' << (it->second ? "ASC" : "DESC");
        }
        out << "</FONT>\n"
            << "             </TD></TR>\n";
    }

    /* Projections */
    if (projection_is_anti() or not projections_.empty()) {
        out << "             <TR><TD ALIGN=\"LEFT\">\n"
            << "               <B>π</B><FONT POINT-SIZE=\"9\">";
        if (projection_is_anti())
            out << "*";
        for (auto it = projections_.begin(), end = projections_.end(); it != end; ++it) {
            if (it != projections_.begin() or projection_is_anti())
                out << ", ";
            out << html_escape(to_string(*it->first));
            if (it->second)
                out << " AS " << html_escape(it->second);
        }
        out << "</FONT>\n"
            << "             </TD></TR>\n";
    }

    /* Group by and aggregates */
    if (not group_by_.empty() or not aggregates_.empty()) {
        out << "             <TR><TD ALIGN=\"LEFT\">\n"
            << "               <B>γ</B><FONT POINT-SIZE=\"9\">";
        for (auto it = group_by_.begin(), end = group_by_.end(); it != end; ++it) {
            if (it != group_by_.begin()) out << ", ";
            out << html_escape(to_string(**it));
        }
        if (not group_by_.empty() and not aggregates_.empty())
            out << ", ";
        for (auto it = aggregates_.begin(), end = aggregates_.end(); it != end; ++it) {
            if (it != aggregates_.begin()) out << ", ";
            out << html_escape(to_string(**it));
        }
        out << "</FONT>\n"
            << "             </TD></TR>\n";
    }

    out << "           </TABLE>\n"
        << "          >;\n"
        << "  }\n";
#undef id
#undef q
}

void QueryGraph::dump(std::ostream &out) const
{
    out << "QueryGraph {\n  sources:";

    /*----- Print sources. -------------------------------------------------------------------------------------------*/
    for (auto src : sources()) {
        out << "\n    ";
        if (auto q = cast<Query>(src)) {
            out << "(...)";
            if (q->alias())
                out << " AS " << q->alias();
        } else {
            auto bt = as<BaseTable>(src);
            out << bt->table().name;
            if (bt->alias() != bt->table().name)
                out << " AS " << src->alias();
        }
        if (not src->filter().empty())
            out << " WHERE " << src->filter();
    }

    /*----- Print joins. ---------------------------------------------------------------------------------------------*/
    if (joins().empty()) {
        out << "\n  no joins";
    } else {
        out << "\n  joins:";
        for (auto j : joins()) {
            out << "\n    {";
            auto &srcs = j->sources();
            for (auto it = srcs.begin(), end = srcs.end(); it != end; ++it) {
                if (it != srcs.begin()) out << ' ';
                out << (*it)->alias();
            }
            out << "}  " << j->condition();
        }
    }

    /*----- Print grouping and aggregation information.  -------------------------------------------------------------*/
    if (group_by().empty() and aggregates().empty()) {
        out << "\n  no grouping";
    } else {
        if (not group_by().empty()) {
            out << "\n  group by: ";
            for (auto it = group_by().begin(), end = group_by().end(); it != end; ++it) {
                if (it != group_by().begin())
                    out << ", ";
                out << **it;
            }
        }
        if (not aggregates().empty()) {
            out << "\n  aggregates: ";
            for (auto it = aggregates().begin(), end = aggregates().end(); it != end; ++it) {
                if (it != aggregates().begin())
                    out << ", ";
                out << **it;
            }
        }
    }

    /*----- Print ordering information. ------------------------------------------------------------------------------*/
    if (order_by().empty()) {
        out << "\n  no order";
    } else {
        out << "\n  order by: ";
        for (auto it = order_by().begin(), end = order_by().end(); it != end; ++it) {
            if (it != order_by().begin())
                out << ", ";
            out << *it->first << ' ' << (it->second ? "ASC" : "DESC");
        }
    }

    /*----- Print projections. ---------------------------------------------------------------------------------------*/
    out << "\n  projections: ";
    for (auto &p : projections()) {
        if (p.second) {
            out << "\n    AS " << p.second;
            ASTDumper P(out, 3);
            P(*p.first);
        } else {
            ASTDumper P(out, 2);
            P(*p.first);
        }
    }

    out << "\n}" << std::endl;
}
void QueryGraph::dump() const { dump(std::cerr); }

void AdjacencyMatrix::dump(std::ostream &out) const { out << *this << std::endl; }
void AdjacencyMatrix::dump() const { dump(std::cerr); }
