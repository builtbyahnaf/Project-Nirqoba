#include "nirqoba/Frontend/AstPrinter.h"

#include <sstream>

namespace nirq {
namespace {

class Printer {
 public:
  std::string print(const Program& program) {
    out_ << "program\n";
    if (program.module_decl()) {
      out_ << "  module @" << program.module_decl()->name() << "\n";
    }
    for (const auto& imp : program.imports()) {
      out_ << "  import @" << imp->module_name();
      if (imp->is_selective()) out_ << "." << imp->item_name();
      if (imp->is_aliased()) out_ << " as " << imp->alias();
      out_ << "\n";
    }
    for (const auto& str : program.structs()) {
      print_struct(*str);
    }
    for (const auto& function : program.functions()) {
      print_function(*function);
    }
    return out_.str();
  }

 private:
  void indent() {
    for (int level = 0; level < depth_; ++level) out_ << "  ";
  }

  void print_struct(const StructDecl& str) {
    indent();
    out_ << visibility_spelling(str.visibility()) << " struct @" << str.name()
         << " (" << str.location().line << ":" << str.location().column << ")\n";
    ++depth_;
    for (const auto& field : str.fields()) {
      indent();
      out_ << "field " << field.name << " : " << field.type.to_string() << "\n";
    }
    --depth_;
  }

  void print_function(const FunctionDecl& function) {
    indent();
    out_ << visibility_spelling(function.visibility()) << " function @" << function.name()
         << " : " << function.return_type().to_string();
    if (!function.return_type().is_valid()) out_ << " (error)";
    out_ << " (" << function.location().line << ":" << function.location().column << ")\n";
    ++depth_;
    if (function.parameters().empty()) {
      indent();
      out_ << "params: none\n";
    }
    for (const Parameter& parameter : function.parameters()) {
      indent();
      out_ << "param " << parameter.name << " : " << parameter.type.to_string() << "\n";
    }
    print_block(function.body());
    --depth_;
  }

  void print_block(const BlockStmt& block) {
    indent();
    out_ << "block\n";
    ++depth_;
    for (const StmtPtr& statement : block.statements()) {
      print_statement(*statement);
    }
    --depth_;
  }

  void print_statement(const Stmt& statement) {
    switch (statement.kind()) {
      case Stmt::Kind::Block:
        print_block(static_cast<const BlockStmt&>(statement));
        return;
      case Stmt::Kind::VariableDeclaration: {
        const auto& declaration = static_cast<const VariableDeclarationStmt&>(statement);
        indent();
        out_ << binding_kind_spelling(declaration.binding()) << " " << declaration.name();
        if (declaration.has_annotation()) {
          out_ << " : " << declaration.annotation().to_string();
        }
        out_ << " = " << expression(declaration.initializer()) << "\n";
        return;
      }
      case Stmt::Kind::Destructuring: {
        const auto& destructure = static_cast<const DestructuringStmt&>(statement);
        indent();
        out_ << binding_kind_spelling(destructure.binding()) << " (";
        for (std::size_t i = 0; i < destructure.names().size(); ++i) {
          if (i > 0) out_ << ", ";
          out_ << destructure.names()[i];
        }
        out_ << ") = " << expression(destructure.value()) << "\n";
        return;
      }
      case Stmt::Kind::Assignment: {
        const auto& assignment = static_cast<const AssignmentStmt&>(statement);
        indent();
        out_ << "assign " << (assignment.is_simple_name() ? assignment.name() : expression(assignment.target()))
             << " = " << expression(assignment.value()) << "\n";
        return;
      }
      case Stmt::Kind::CompoundAssignment: {
        const auto& compound = static_cast<const CompoundAssignmentStmt&>(statement);
        indent();
        out_ << "assign " << (compound.is_simple_name() ? compound.name() : expression(compound.target()))
             << " " << binary_op_spelling(compound.op()) << "= "
             << expression(compound.value()) << "\n";
        return;
      }
      case Stmt::Kind::If: {
        const auto& if_stmt = static_cast<const IfStmt&>(statement);
        indent();
        out_ << "if " << expression(if_stmt.condition()) << "\n";
        print_block(if_stmt.then_branch());
        if (if_stmt.has_else()) {
          indent();
          out_ << "else\n";
          print_statement(*if_stmt.else_branch());
        }
        return;
      }
      case Stmt::Kind::While: {
        const auto& while_stmt = static_cast<const WhileStmt&>(statement);
        indent();
        out_ << "while " << expression(while_stmt.condition()) << "\n";
        print_block(while_stmt.body());
        return;
      }
      case Stmt::Kind::For: {
        const auto& for_stmt = static_cast<const ForStmt&>(statement);
        indent();
        out_ << "for\n";
        print_block(for_stmt.body());
        return;
      }
      case Stmt::Kind::Break:
        indent();
        out_ << "break\n";
        return;
      case Stmt::Kind::Continue:
        indent();
        out_ << "continue\n";
        return;
      case Stmt::Kind::Return: {
        const auto& return_statement = static_cast<const ReturnStmt&>(statement);
        indent();
        out_ << "return";
        if (return_statement.has_value()) out_ << " " << expression(*return_statement.value());
        out_ << "\n";
        return;
      }
      case Stmt::Kind::Expression: {
        const auto& expression_statement = static_cast<const ExprStmt&>(statement);
        indent();
        out_ << "expr " << expression(expression_statement.expr()) << "\n";
        return;
      }
    }
  }

  std::string expression(const Expr& expr) {
    switch (expr.kind()) {
      case Expr::Kind::IntegerLiteral:
        return std::to_string(static_cast<const IntegerLiteralExpr&>(expr).value());
      case Expr::Kind::FloatLiteral:
        return std::to_string(static_cast<const FloatLiteralExpr&>(expr).value());
      case Expr::Kind::CharLiteral:
        return "'" + std::to_string(static_cast<const CharLiteralExpr&>(expr).value()) + "'";
      case Expr::Kind::StringLiteral: {
        const std::string& value = static_cast<const StringLiteralExpr&>(expr).value();
        std::string escaped;
        for (const char c : value) {
          switch (c) {
            case '\n': escaped += "\\n"; break;
            case '\t': escaped += "\\t"; break;
            case '\r': escaped += "\\r"; break;
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            default: escaped.push_back(c); break;
          }
        }
        return "\"" + escaped + "\"";
      }
      case Expr::Kind::BoolLiteral:
        return static_cast<const BoolLiteralExpr&>(expr).value() ? "true" : "false";
      case Expr::Kind::ArrayLiteral: {
        const auto& arr = static_cast<const ArrayLiteralExpr&>(expr);
        std::string text = "[";
        for (std::size_t i = 0; i < arr.elements().size(); ++i) {
          if (i > 0) text += ", ";
          text += expression(*arr.elements()[i]);
        }
        return text + "]";
      }
      case Expr::Kind::TupleLiteral: {
        const auto& tup = static_cast<const TupleLiteralExpr&>(expr);
        std::string text = "(";
        for (std::size_t i = 0; i < tup.elements().size(); ++i) {
          if (i > 0) text += ", ";
          text += expression(*tup.elements()[i]);
        }
        return text + ")";
      }
      case Expr::Kind::Index: {
        const auto& idx = static_cast<const IndexExpr&>(expr);
        return expression(idx.base()) + "[" + expression(idx.index()) + "]";
      }
      case Expr::Kind::MemberAccess: {
        const auto& mem = static_cast<const MemberAccessExpr&>(expr);
        if (mem.is_tuple_access()) {
          return expression(mem.base()) + "." + std::to_string(mem.tuple_index());
        }
        return expression(mem.base()) + "." + mem.member();
      }
      case Expr::Kind::StructInit: {
        const auto& init = static_cast<const StructInitExpr&>(expr);
        std::string text = init.struct_name() + " {";
        if (init.is_positional()) {
          for (std::size_t i = 0; i < init.positional_inits().size(); ++i) {
            if (i > 0) text += ", ";
            text += expression(*init.positional_inits()[i]);
          }
        } else {
          for (std::size_t i = 0; i < init.field_inits().size(); ++i) {
            if (i > 0) text += ", ";
            text += init.field_inits()[i].first + ": " + expression(*init.field_inits()[i].second);
          }
        }
        return text + "}";
      }
      case Expr::Kind::Name:
        return static_cast<const NameExpr&>(expr).name();
      case Expr::Kind::Call: {
        const auto& call = static_cast<const CallExpr&>(expr);
        std::string text = call.callee() + "(";
        for (std::size_t index = 0; index < call.arguments().size(); ++index) {
          if (index != 0) text += ", ";
          text += expression(*call.arguments()[index]);
        }
        return text + ")";
      }
      case Expr::Kind::Unary: {
        const auto& unary = static_cast<const UnaryExpr&>(expr);
        return "(" + std::string(unary_op_spelling(unary.op())) + expression(unary.operand()) + ")";
      }
      case Expr::Kind::Binary: {
        const auto& binary = static_cast<const BinaryExpr&>(expr);
        return "(" + expression(binary.lhs()) + " " + std::string(binary_op_spelling(binary.op())) +
               " " + expression(binary.rhs()) + ")";
      }
      case Expr::Kind::Cast: {
        const auto& cast = static_cast<const CastExpr&>(expr);
        return "(" + expression(cast.operand()) + " as " + cast.target_type().to_string() + ")";
      }
    }
    return "<unknown>";
  }

  std::ostringstream out_;
  int depth_ = 0;
};

}  // namespace

std::string print_program(const Program& program) {
  Printer printer;
  return printer.print(program);
}

std::string print_ast(const Program& program) {
  return print_program(program);
}

}  // namespace nirq
