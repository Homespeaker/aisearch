from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path


DATA_FILE = Path(__file__).with_name("tasks.json")


@dataclass
class Task:
    task_id: int
    title: str
    difficulty: int
    urgency: int
    duration_minutes: int
    completed: bool = False
    created_at: str = ""

    @property
    def priority(self) -> int:
        return self.urgency * 2 + self.difficulty + short_task_bonus(self.duration_minutes)


def short_task_bonus(duration_minutes: int) -> int:
    if duration_minutes <= 15:
        return 3
    if duration_minutes <= 30:
        return 2
    if duration_minutes <= 60:
        return 1
    return 0


def priority_reason(task: Task) -> str:
    reasons: list[str] = []

    if task.urgency >= 4:
        reasons.append("высокая срочность")
    elif task.urgency >= 2:
        reasons.append("средняя срочность")

    if task.difficulty >= 4:
        reasons.append("задача требует хорошей концентрации")

    if task.duration_minutes <= 30:
        reasons.append(f"ее можно закрыть примерно за {task.duration_minutes} минут")
    elif task.duration_minutes <= 60:
        reasons.append("она не займет слишком много времени")

    if not reasons:
        reasons.append("это сейчас самый сбалансированный вариант")

    return " и ".join(reasons)


class TaskStorage:
    def __init__(self, data_file: Path) -> None:
        self.data_file = data_file

    def load_tasks(self) -> list[Task]:
        if not self.data_file.exists():
            return []

        try:
            with self.data_file.open("r", encoding="utf-8") as file:
                raw_tasks = json.load(file)
        except (json.JSONDecodeError, OSError):
            return []

        return [Task(**item) for item in raw_tasks]

    def save_tasks(self, tasks: list[Task]) -> None:
        payload = [asdict(task) for task in tasks]
        with self.data_file.open("w", encoding="utf-8") as file:
            json.dump(payload, file, ensure_ascii=False, indent=2)


class SmartTodoApp:
    def __init__(self) -> None:
        self.storage = TaskStorage(DATA_FILE)
        self.tasks = self.storage.load_tasks()

    def run(self) -> None:
        print("SmartTodo")
        print("Умный список дел в консоли")

        while True:
            print("\nВыбери действие:")
            print("1. Добавить задачу")
            print("2. Показать все задачи")
            print("3. Получить совет")
            print("4. Отметить задачу выполненной")
            print("5. Выйти")

            choice = input("Введите номер: ").strip()

            if choice == "1":
                self.add_task()
            elif choice == "2":
                self.show_tasks()
            elif choice == "3":
                self.show_recommendation()
            elif choice == "4":
                self.complete_task()
            elif choice == "5":
                print("Список дел сохранен. До встречи!")
                break
            else:
                print("Не понял выбор. Попробуй ввести число от 1 до 5.")

    def add_task(self) -> None:
        print("\nДобавление задачи")
        title = self.prompt_non_empty("Название задачи: ")
        difficulty = self.prompt_int("Сложность (1-5): ", min_value=1, max_value=5)
        urgency = self.prompt_int("Срочность (1-5): ", min_value=1, max_value=5)
        duration_minutes = self.prompt_int("Время выполнения в минутах: ", min_value=1)

        task = Task(
            task_id=self.next_task_id(),
            title=title,
            difficulty=difficulty,
            urgency=urgency,
            duration_minutes=duration_minutes,
            completed=False,
            created_at=datetime.now().isoformat(timespec="minutes"),
        )

        self.tasks.append(task)
        self.storage.save_tasks(self.tasks)

        print("\nЗадача добавлена.")
        print(f"Приоритет: {task.priority}")
        print(f"Причина: {priority_reason(task)}")

    def show_tasks(self) -> None:
        if not self.tasks:
            print("\nЗадач пока нет.")
            return

        print("\nВсе задачи:")
        for index, task in enumerate(self.sorted_tasks(self.tasks), start=1):
            status = "выполнено" if task.completed else "активна"
            print(
                f"{index}. [{task.task_id}] {task.title} | "
                f"статус: {status} | "
                f"приоритет: {task.priority} | "
                f"срочность: {task.urgency}/5 | "
                f"сложность: {task.difficulty}/5 | "
                f"время: {task.duration_minutes} мин"
            )

    def show_recommendation(self) -> None:
        active_tasks = self.sorted_tasks(self.active_tasks())
        if not active_tasks:
            print("\nСейчас совет простой: сначала добавь хотя бы одну задачу.")
            return

        best_task = active_tasks[0]
        print("\nСейчас лучше сделать:")
        print(f"1. {best_task.title}")
        print(f"Причина: {priority_reason(best_task)}.")
        print(self.energy_tip(best_task))

    def complete_task(self) -> None:
        active_tasks = self.sorted_tasks(self.active_tasks())
        if not active_tasks:
            print("\nОтмечать пока нечего: активных задач нет.")
            return

        print("\nКакую задачу отметить выполненной?")
        for task in active_tasks:
            print(f"[{task.task_id}] {task.title}")

        task_id = self.prompt_int("Введите ID задачи: ", min_value=1)
        task = self.find_task(task_id)

        if task is None or task.completed:
            print("Задача с таким ID не найдена среди активных.")
            return

        task.completed = True
        self.storage.save_tasks(self.tasks)
        print(f"Готово. Задача \"{task.title}\" отмечена выполненной.")

    def active_tasks(self) -> list[Task]:
        return [task for task in self.tasks if not task.completed]

    def sorted_tasks(self, tasks: list[Task]) -> list[Task]:
        return sorted(
            tasks,
            key=lambda task: (
                not task.completed,
                task.priority,
                task.urgency,
                -task.duration_minutes,
            ),
            reverse=True,
        )

    def next_task_id(self) -> int:
        if not self.tasks:
            return 1
        return max(task.task_id for task in self.tasks) + 1

    def find_task(self, task_id: int) -> Task | None:
        for task in self.tasks:
            if task.task_id == task_id:
                return task
        return None

    def energy_tip(self, task: Task) -> str:
        if task.duration_minutes <= 20:
            return "Подсказка: если времени мало, начни с короткой задачи и быстро получи результат."
        if task.difficulty >= 4:
            return "Подсказка: если есть силы и концентрация, сейчас хороший момент взяться за сложное."
        return "Подсказка: начни с этой задачи, чтобы сохранить темп и постепенно разгрузить список."

    def prompt_non_empty(self, message: str) -> str:
        while True:
            value = input(message).strip()
            if value:
                return value
            print("Поле не должно быть пустым.")

    def prompt_int(self, message: str, min_value: int, max_value: int | None = None) -> int:
        while True:
            raw_value = input(message).strip()
            try:
                value = int(raw_value)
            except ValueError:
                print("Нужно ввести целое число.")
                continue

            if value < min_value:
                print(f"Значение должно быть не меньше {min_value}.")
                continue

            if max_value is not None and value > max_value:
                print(f"Значение должно быть не больше {max_value}.")
                continue

            return value
