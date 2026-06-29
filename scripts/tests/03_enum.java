// Test: Enum type definition
public class EnumTest {

    public enum Day {
        MONDAY, TUESDAY, WEDNESDAY, THURSDAY, FRIDAY, SATURDAY, SUNDAY
    }

    public static void main(String[] args) {
        Day today = Day.MONDAY;
        System.out.println("today = " + today);

        Day friday = Day.FRIDAY;
        System.out.println("friday = " + friday);

        System.out.println("=== Enum: PASS ===");
    }
}
